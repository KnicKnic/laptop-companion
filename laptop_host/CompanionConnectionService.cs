using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Devices.Enumeration;
using Windows.Storage.Streams;

namespace X3LaptopCompanion
{
    public sealed class CompanionConnectionService : IDisposable
    {
        private DeviceWatcher watcher;
        private BluetoothLEAdvertisementWatcher advertisementWatcher;
        private readonly Dictionary<ulong, DateTimeOffset> advertisementLogTimes = new Dictionary<ulong, DateTimeOffset>();
        private BluetoothLEDevice companionDevice;
        private GattSession gattSession;
        private GattDeviceService companionService;
        private GattCharacteristic hostTeamsStateCharacteristic;
        private GattCharacteristic hostMicrophoneStateCharacteristic;
        private GattCharacteristic hostCameraStateCharacteristic;
        private GattCharacteristic hostStatusMessageCharacteristic;
        private GattCharacteristic hostMeetingStateCharacteristic;
        private GattCharacteristic hostHandStateCharacteristic;
        private GattCharacteristic hostMeetingNameCharacteristic;
        private GattCharacteristic hostDesktopStateCharacteristic;
        private GattCharacteristic hostMeetingListCharacteristic;
        private GattCharacteristic buttonEventCharacteristic;
        private GattCharacteristic participationCharacteristic;
        private GattCharacteristic deviceInfoCharacteristic;
        private readonly SemaphoreSlim hostStateWriteLock = new SemaphoreSlim(1, 1);
        private Timer scanWatchdogTimer;
        private int connecting;
        // Each connect attempt gets a generation; the watchdog abandons a hung attempt by
        // bumping it, so a late completion can tell it no longer owns the connection state.
        private int connectGeneration;
        private long connectStartedTicks;
        private static readonly TimeSpan ConnectAttemptTimeout = TimeSpan.FromSeconds(90);
        private bool intentionallyPausedAdvertisementWatcher;
        private bool disposed;
        private uint lastParticipationCounter;
        private DateTimeOffset? lastParticipationReceivedAt;

        public event EventHandler<CompanionConnectionStatus> StatusChanged;
        public event EventHandler<CompanionButtonEvent> ButtonEventReceived;
        public event EventHandler<CompanionParticipationEvent> ParticipationEventReceived;

        public void Start()
        {
            if (watcher != null)
            {
                return;
            }

            var selector = GattDeviceService.GetDeviceSelectorFromUuid(CompanionProtocol.ServiceUuid);
            HostLog.Write("BLE watcher starting. Selector=" + selector);
            watcher = DeviceInformation.CreateWatcher(selector);
            watcher.Added += OnDeviceAdded;
            watcher.Removed += OnDeviceRemoved;
            watcher.EnumerationCompleted += OnWatcherEnumerationCompleted;
            watcher.Stopped += OnWatcherStopped;
            watcher.Start();
            HostLog.Write("BLE watcher started. Status=" + watcher.Status);

            advertisementWatcher = new BluetoothLEAdvertisementWatcher
            {
                ScanningMode = BluetoothLEScanningMode.Active
            };
            advertisementWatcher.AdvertisementFilter.Advertisement.ServiceUuids.Add(CompanionProtocol.ServiceUuid);
            advertisementWatcher.Received += OnAdvertisementReceived;
            advertisementWatcher.Stopped += OnAdvertisementWatcherStopped;
            advertisementWatcher.Start();
            HostLog.Write("BLE advertisement watcher started. Status=" + advertisementWatcher.Status +
                " ServiceUuid=" + CompanionProtocol.ServiceUuid);
            scanWatchdogTimer = new Timer(OnScanWatchdogTick, null, TimeSpan.FromSeconds(10), TimeSpan.FromSeconds(10));
            PublishStatus(false, "Scanning for X3 companion service.");
        }

        public void Stop()
        {
            if (watcher != null)
            {
                watcher.Added -= OnDeviceAdded;
                watcher.Removed -= OnDeviceRemoved;
                watcher.EnumerationCompleted -= OnWatcherEnumerationCompleted;
                watcher.Stopped -= OnWatcherStopped;
                if (watcher.Status == DeviceWatcherStatus.Started ||
                    watcher.Status == DeviceWatcherStatus.EnumerationCompleted)
                {
                    watcher.Stop();
                }

                watcher = null;
            }

            if (advertisementWatcher != null)
            {
                advertisementWatcher.Received -= OnAdvertisementReceived;
                advertisementWatcher.Stopped -= OnAdvertisementWatcherStopped;
                intentionallyPausedAdvertisementWatcher = false;
                if (advertisementWatcher.Status == BluetoothLEAdvertisementWatcherStatus.Started)
                {
                    advertisementWatcher.Stop();
                }

                advertisementWatcher = null;
            }

            scanWatchdogTimer?.Dispose();
            scanWatchdogTimer = null;
            // Orphan any in-flight connect so it cannot adopt state after the service stopped.
            Interlocked.Increment(ref connectGeneration);
            Interlocked.Exchange(ref connecting, 0);
            ResetGattState();
            HostLog.Write("BLE watcher/service stopped.");
            PublishStatus(false, "BLE scanning stopped.");
        }

        public void Dispose()
        {
            disposed = true;
            Stop();
        }

        public async Task<bool> SendHostStatusAsync(bool teamsDetected, bool meetingDetected,
            string meetingName, CompanionTriState microphone, CompanionTriState camera, CompanionTriState hand,
            string message, ushort teamsCounter = 0, ushort meetingCounter = 0, ushort microphoneCounter = 0,
            ushort cameraCounter = 0, ushort handCounter = 0, bool cameraLocked = false, bool handLocked = false,
            CompanionButton? priorityButton = null)
        {
            if (!await hostStateWriteLock.WaitAsync(0))
            {
                HostLog.Write("Host state skipped; previous BLE write is still pending.");
                return false;
            }

            try
            {
                if (hostTeamsStateCharacteristic == null || hostMeetingStateCharacteristic == null ||
                    hostMeetingNameCharacteristic == null || hostMicrophoneStateCharacteristic == null ||
                    hostCameraStateCharacteristic == null || hostHandStateCharacteristic == null ||
                    hostStatusMessageCharacteristic == null)
                {
                    HostLog.Write("Host state skipped; one or more state characteristics are not available.");
                    return false;
                }

                var startedAt = DateTimeOffset.Now;
                HostLog.Write("Host state write starting option=WriteWithoutResponse teams=" + teamsDetected +
                    " meeting=" + meetingDetected + " meetingName=" + meetingName +
                    " mic=" + microphone + " camera=" + camera +
                    " cameraLocked=" + cameraLocked + " hand=" + hand + " handLocked=" + handLocked +
                    " counters=" + teamsCounter + "/" + meetingCounter + "/" +
                    microphoneCounter + "/" + cameraCounter + "/" + handCounter +
                    " priorityButton=" + (priorityButton.HasValue ? priorityButton.Value.ToString() : "(none)") +
                    " message=" + message);
                var teamsStatus = GattCommunicationStatus.Success;
                var meetingStatus = GattCommunicationStatus.Success;
                var meetingNameStatus = GattCommunicationStatus.Success;
                var microphoneStatus = GattCommunicationStatus.Success;
                var cameraStatus = GattCommunicationStatus.Success;
                var handStatus = GattCommunicationStatus.Success;

                if (priorityButton == CompanionButton.ToggleMute)
                {
                    microphoneStatus = await WriteEncodedStateAsync(hostMicrophoneStateCharacteristic,
                        CompanionProtocol.TriStateIsOn(microphone), microphoneCounter, "microphone");
                }
                else if (priorityButton == CompanionButton.ToggleCamera)
                {
                    cameraStatus = await WriteEncodedStateAsync(hostCameraStateCharacteristic,
                        CompanionProtocol.TriStateIsOn(camera), cameraCounter, "camera", cameraLocked);
                }
                else if (priorityButton == CompanionButton.ToggleHand)
                {
                    handStatus = await WriteEncodedStateAsync(hostHandStateCharacteristic,
                        CompanionProtocol.TriStateIsOn(hand), handCounter, "hand", handLocked);
                }

                teamsStatus = await WriteEncodedStateAsync(hostTeamsStateCharacteristic, teamsDetected,
                    teamsCounter, "teams");
                meetingStatus = await WriteEncodedStateAsync(hostMeetingStateCharacteristic,
                    meetingDetected, meetingCounter, "meeting");
                meetingNameStatus = await WriteStringStateAsync(hostMeetingNameCharacteristic, meetingName, "meeting name");
                if (priorityButton != CompanionButton.ToggleMute)
                {
                    microphoneStatus = await WriteEncodedStateAsync(hostMicrophoneStateCharacteristic,
                        CompanionProtocol.TriStateIsOn(microphone), microphoneCounter, "microphone");
                }

                if (priorityButton != CompanionButton.ToggleCamera)
                {
                    cameraStatus = await WriteEncodedStateAsync(hostCameraStateCharacteristic,
                        CompanionProtocol.TriStateIsOn(camera), cameraCounter, "camera", cameraLocked);
                }

                if (priorityButton != CompanionButton.ToggleHand)
                {
                    handStatus = await WriteEncodedStateAsync(hostHandStateCharacteristic,
                        CompanionProtocol.TriStateIsOn(hand), handCounter, "hand", handLocked);
                }

                var messageStatus = await WriteStringStateAsync(hostStatusMessageCharacteristic, message, "message");
                var elapsedMs = (DateTimeOffset.Now - startedAt).TotalMilliseconds;
                HostLog.Write("Host state write complete teamsStatus=" + teamsStatus + " micStatus=" + microphoneStatus +
                    " meetingStatus=" + meetingStatus + " meetingNameStatus=" + meetingNameStatus +
                    " cameraStatus=" + cameraStatus +
                    " handStatus=" + handStatus + " messageStatus=" + messageStatus +
                    " elapsedMs=" + elapsedMs.ToString("F0"));
                if (teamsStatus != GattCommunicationStatus.Success ||
                    meetingStatus != GattCommunicationStatus.Success ||
                    meetingNameStatus != GattCommunicationStatus.Success ||
                    microphoneStatus != GattCommunicationStatus.Success ||
                    cameraStatus != GattCommunicationStatus.Success ||
                    handStatus != GattCommunicationStatus.Success ||
                    messageStatus != GattCommunicationStatus.Success)
                {
                    ResetGattState();
                    RestartAdvertisementWatcherIfNeeded("host state write failed");
                    PublishStatus(false, "Host state write failed.");
                    return false;
                }

                return true;
            }
            catch (Exception ex)
            {
                HostLog.Write("Host state write failed with exception.", ex);
                ResetGattState();
                RestartAdvertisementWatcherIfNeeded("host state write exception");
                PublishStatus(false, "Host state write exception: " + ex.Message);
                return false;
            }
            finally
            {
                hostStateWriteLock.Release();
            }
        }

        private static async Task<GattCommunicationStatus> WriteEncodedStateAsync(GattCharacteristic characteristic,
            bool on, ushort counter, string name, bool locked = false)
        {
            var writer = new DataWriter
            {
                ByteOrder = ByteOrder.LittleEndian
            };
            var encoded = CompanionProtocol.EncodeState(on, counter, locked);
            writer.WriteUInt16(encoded);
            var buffer = writer.DetachBuffer();
            HostLog.Write("Host state write " + name + " bytes=" + buffer.Length +
                " properties=" + characteristic.CharacteristicProperties);
            var status = await characteristic.WriteValueAsync(buffer, GattWriteOption.WriteWithoutResponse);
            HostLog.Write("Host state write " + name + " status=" + status + " on=" + on +
                " locked=" + locked + " counter=" + counter + " encoded=" + encoded);
            return status;
        }

        private static async Task<GattCommunicationStatus> WriteStringStateAsync(GattCharacteristic characteristic, string value, string name)
        {
            var writer = new DataWriter
            {
                ByteOrder = ByteOrder.LittleEndian
            };
            var message = string.IsNullOrWhiteSpace(value) ? string.Empty : value;
            writer.WriteString(message.Length > 48 ? message.Substring(0, 48) : message);
            var buffer = writer.DetachBuffer();
            HostLog.Write("Host state write " + name + " bytes=" + buffer.Length +
                " properties=" + characteristic.CharacteristicProperties + " value=" + message);
            var status = await characteristic.WriteValueAsync(buffer, GattWriteOption.WriteWithoutResponse);
            HostLog.Write("Host state write " + name + " status=" + status);
            return status;
        }

        public async Task<bool> SendDesktopStateAsync(CompanionDesktopState desktops)
        {
            if (desktops == null)
            {
                return false;
            }

            if (!await hostStateWriteLock.WaitAsync(0))
            {
                HostLog.Write("Desktop state skipped; previous BLE write is still pending.");
                return false;
            }

            try
            {
                var characteristic = hostDesktopStateCharacteristic;
                if (characteristic == null)
                {
                    HostLog.Write("Desktop state skipped; characteristic is not available.");
                    return false;
                }

                var payload = CompanionProtocol.EncodeDesktopState(desktops, MaxWritePayloadBytes());
                var writer = new DataWriter
                {
                    ByteOrder = ByteOrder.LittleEndian
                };
                writer.WriteBytes(payload);
                var buffer = writer.DetachBuffer();
                var status = await characteristic.WriteValueAsync(buffer, GattWriteOption.WriteWithoutResponse);
                HostLog.Write("Desktop state write status=" + status + " bytes=" + buffer.Length +
                    " desktops=" + desktops.Describe());
                if (status != GattCommunicationStatus.Success)
                {
                    ResetGattState();
                    RestartAdvertisementWatcherIfNeeded("desktop state write failed");
                    PublishStatus(false, "Desktop state write failed.");
                    return false;
                }

                return true;
            }
            catch (Exception ex)
            {
                HostLog.Write("Desktop state write failed with exception.", ex);
                ResetGattState();
                RestartAdvertisementWatcherIfNeeded("desktop state write exception");
                PublishStatus(false, "Desktop state write exception: " + ex.Message);
                return false;
            }
            finally
            {
                hostStateWriteLock.Release();
            }
        }

        public async Task<bool> SendMeetingListAsync(CompanionMeetingPage page)
        {
            if (!await hostStateWriteLock.WaitAsync(0)) return false;
            try
            {
                var characteristic = hostMeetingListCharacteristic;
                if (characteristic == null)
                {
                    HostLog.Write("Meeting list skipped; characteristic unavailable.");
                    return false;
                }
                var payload = CompanionProtocol.EncodeMeetingPage(page);
                foreach (var fragment in CompanionProtocol.MeetingFragments(payload, MaxWritePayloadBytes()))
                {
                    using var writer = new DataWriter();
                    writer.WriteBytes(fragment);
                    var status = await characteristic.WriteValueAsync(writer.DetachBuffer(), GattWriteOption.WriteWithResponse);
                    if (status != GattCommunicationStatus.Success)
                        throw new InvalidOperationException("Meeting list write failed: " + status);
                }
                return true;
            }
            catch (Exception ex)
            {
                HostLog.Write("Meeting list write failed.", ex);
                ResetGattState();
                RestartAdvertisementWatcherIfNeeded("meeting list write failed");
                PublishStatus(false, "Meeting list write failed: " + ex.Message);
                return false;
            }
            finally
            {
                hostStateWriteLock.Release();
            }
        }

        private int MaxWritePayloadBytes()
        {
            const int fallback = 20;
            var session = gattSession;
            if (session == null)
            {
                return fallback;
            }

            try
            {
                // WriteWithoutResponse can carry MaxPduSize minus the 3 byte ATT header.
                var usable = session.MaxPduSize - 3;
                return usable < fallback ? fallback : usable;
            }
            catch (Exception ex)
            {
                HostLog.Write("GATT MTU lookup failed; using conservative payload size.", ex);
                return fallback;
            }
        }

        private async void OnDeviceAdded(DeviceWatcher sender, DeviceInformation args)        {
            HostLog.Write("BLE device added. Name=" + args.Name + " IsPaired=" + args.Pairing.IsPaired + " Id=" + args.Id);
            if (IsConnected())
            {
                HostLog.Write("BLE device add ignored; already connected.");
                return;
            }

            if (!TryBeginConnect(out var generation))
            {
                HostLog.Write("BLE device add ignored. disposed=" + disposed + " connecting=" + connecting);
                return;
            }

            try
            {
                await ConnectAsync(args, generation);
            }
            catch (Exception ex)
            {
                HostLog.Write("BLE connect failed with exception.", ex);
                if (IsCurrentAttempt(generation))
                {
                    PublishStatus(false, "BLE connect exception: " + ex.Message);
                }
            }
            finally
            {
                if (IsCurrentAttempt(generation))
                {
                    if (!IsConnected())
                    {
                        ResetGattState();
                        RestartAdvertisementWatcherIfNeeded("device add not connected");
                    }

                    Interlocked.Exchange(ref connecting, 0);
                }
                else
                {
                    HostLog.Write("Abandoned BLE device connect attempt finished. generation=" + generation);
                }
            }
        }

        private void OnDeviceRemoved(DeviceWatcher sender, DeviceInformationUpdate args)
        {
            HostLog.Write("BLE device removed. Id=" + args.Id);
            ResetGattState();
            RestartAdvertisementWatcherIfNeeded("device removed");
            PublishStatus(false, "X3 companion disconnected.");
        }

        private void OnWatcherEnumerationCompleted(DeviceWatcher sender, object args)
        {
            HostLog.Write("BLE watcher enumeration completed. Status=" + sender.Status);
            PublishStatus(false, "BLE scan enumeration completed; still watching for X3.");
        }

        private void OnWatcherStopped(DeviceWatcher sender, object args)
        {
            HostLog.Write("BLE watcher stopped. Status=" + sender.Status);
            if (!disposed)
            {
                PublishStatus(false, "BLE watcher stopped.");
            }
        }

        private async void OnAdvertisementReceived(BluetoothLEAdvertisementWatcher sender, BluetoothLEAdvertisementReceivedEventArgs args)
        {
            if (disposed)
            {
                return;
            }

            LogAdvertisement(args);
            if (IsConnected())
            {
                HostLog.Write("Companion advertisement received while host believes it is connected; resetting stale GATT state.");
                ResetGattState();
            }

            if (!TryBeginConnect(out var generation))
            {
                return;
            }

            try
            {
                await ConnectFromAdvertisementAsync(args.BluetoothAddress, args.BluetoothAddressType, generation);
            }
            catch (COMException ex)
            {
                HostLog.Write("BLE advertisement connect failed with COM exception. HResult=0x" +
                    ex.HResult.ToString("X8"), ex);
                if (IsCurrentAttempt(generation))
                {
                    PublishStatus(false, "BLE GATT connect failed: 0x" + ex.HResult.ToString("X8"));
                }
            }
            catch (Exception ex)
            {
                HostLog.Write("BLE advertisement connect failed with exception.", ex);
                if (IsCurrentAttempt(generation))
                {
                    PublishStatus(false, "BLE advertisement connect exception: " + ex.Message);
                }
            }
            finally
            {
                if (IsCurrentAttempt(generation))
                {
                    if (!IsConnected())
                    {
                        ResetGattState();
                    }

                    Interlocked.Exchange(ref connecting, 0);
                    RestartAdvertisementWatcherIfNeeded("advertisement connect finally");
                }
                else
                {
                    HostLog.Write("Abandoned BLE advertisement connect attempt finished. generation=" + generation);
                }
            }
        }

        private void OnAdvertisementWatcherStopped(BluetoothLEAdvertisementWatcher sender, BluetoothLEAdvertisementWatcherStoppedEventArgs args)
        {
            HostLog.Write("BLE advertisement watcher stopped. Status=" + sender.Status + " BluetoothStatus=" + args.Error);
            if (intentionallyPausedAdvertisementWatcher)
            {
                return;
            }

            if (!disposed)
            {
                PublishStatus(false, "BLE advertisement watcher stopped: " + args.Error);
            }
        }

        private void OnGattSessionStatusChanged(GattSession sender, GattSessionStatusChangedEventArgs args)
        {
            HostLog.Write("GattSession status changed. Status=" + args.Status + " BluetoothStatus=" + args.Error);
            if (disposed || connecting != 0 || args.Status != GattSessionStatus.Closed)
            {
                return;
            }

            ResetGattState();
            RestartAdvertisementWatcherIfNeeded("gatt session closed");
            PublishStatus(false, "X3 companion GATT session closed.");
        }

        private async Task ConnectAsync(DeviceInformation deviceInfo, int generation)
        {
            PublishStatus(false, "Found X3 companion service. Connecting...");
            HostLog.Write("Opening GATT service. Name=" + deviceInfo.Name + " Id=" + deviceInfo.Id);

            var service = await GattDeviceService.FromIdAsync(deviceInfo.Id);
            if (service == null)
            {
                HostLog.Write("GattDeviceService.FromIdAsync returned null.");
                PublishStatus(false, "Unable to open X3 companion service.");
                return;
            }

            await UseGattServiceAsync(service, generation);
        }

        private async Task ConnectFromAdvertisementAsync(ulong bluetoothAddress, BluetoothAddressType addressType,
            int generation)
        {
            PublishStatus(false, "Found X3 companion advertisement. Connecting...");
            HostLog.Write("Opening BLE device from advertisement. Address=" + FormatBluetoothAddress(bluetoothAddress) +
                " AddressType=" + addressType);

            if (advertisementWatcher?.Status == BluetoothLEAdvertisementWatcherStatus.Started)
            {
                intentionallyPausedAdvertisementWatcher = true;
                advertisementWatcher.Stop();
                HostLog.Write("BLE advertisement watcher paused for GATT connection attempt.");
            }

            BluetoothLEDevice device = null;
            GattSession session = null;
            var transferred = false;
            try
            {
                device = await BluetoothLEDevice.FromBluetoothAddressAsync(bluetoothAddress, addressType);
                if (device == null)
                {
                    HostLog.Write("BluetoothLEDevice.FromBluetoothAddressAsync returned null for " +
                        FormatBluetoothAddress(bluetoothAddress) + " AddressType=" + addressType);
                    PublishStatus(false, "Unable to open X3 BLE device from advertisement.");
                    RestartAdvertisementWatcherIfNeeded("advertisement device null");
                    return;
                }

                HostLog.Write("BLE device opened. Name=" + device.Name + " Address=" +
                    FormatBluetoothAddress(device.BluetoothAddress) + " AddressType=" + device.BluetoothAddressType +
                    " ConnectionStatus=" + device.ConnectionStatus + " CanPair=" + device.DeviceInformation.Pairing.CanPair +
                    " IsPaired=" + device.DeviceInformation.Pairing.IsPaired);

                session = await CreateMaintainedGattSessionAsync(device);
                var services = await GetGattServicesWithRetryAsync(device);
                HostLog.Write("Advertisement GATT service lookup status=" + services.Status + " count=" + services.Services.Count);
                if (!IsCurrentAttempt(generation))
                {
                    foreach (var abandonedService in services.Services)
                    {
                        abandonedService.Dispose();
                    }

                    return;
                }

                if (services.Status != GattCommunicationStatus.Success || services.Services.Count == 0)
                {
                    PublishStatus(false, "X3 advertisement found, but GATT service could not be opened: " +
                        services.Status + " count=" + services.Services.Count);
                    RestartAdvertisementWatcherIfNeeded("gatt service lookup failed");
                    return;
                }

                if (gattSession != null)
                {
                    gattSession.SessionStatusChanged -= OnGattSessionStatusChanged;
                    gattSession.Dispose();
                }

                gattSession = session;
                if (gattSession != null)
                {
                    gattSession.SessionStatusChanged += OnGattSessionStatusChanged;
                }

                companionDevice?.Dispose();
                companionDevice = device;
                transferred = true;
                await UseGattServiceAsync(services.Services[0], generation);
            }
            finally
            {
                if (!transferred)
                {
                    session?.Dispose();
                    device?.Dispose();
                }
            }
        }

        private static async Task<GattSession> CreateMaintainedGattSessionAsync(BluetoothLEDevice device)
        {
            var session = await GattSession.FromDeviceIdAsync(device.BluetoothDeviceId);
            if (session == null)
            {
                HostLog.Write("GattSession.FromDeviceIdAsync returned null.");
                return null;
            }

            HostLog.Write("GattSession opened. Status=" + session.SessionStatus + " CanMaintainConnection=" +
                session.CanMaintainConnection + " MaxPduSize=" + session.MaxPduSize);
            if (session.CanMaintainConnection)
            {
                session.MaintainConnection = true;
                HostLog.Write("GattSession MaintainConnection enabled. Status=" + session.SessionStatus);
                await Task.Delay(250);
            }

            return session;
        }

        private static async Task<GattDeviceServicesResult> GetGattServicesWithRetryAsync(BluetoothLEDevice device)
        {
            GattDeviceServicesResult lastResult = null;
            COMException lastComException = null;
            for (var attempt = 1; attempt <= 10; attempt++)
            {
                try
                {
                    var cacheMode = attempt <= 4 || attempt % 3 == 0 ? BluetoothCacheMode.Uncached : BluetoothCacheMode.Cached;
                    var services = await GetGattServicesForUuidLoggedAsync(device, cacheMode, "attempt " + attempt);
                    lastResult = services;
                    lastComException = null;
                    if (services.Status == GattCommunicationStatus.Success && services.Services.Count > 0)
                    {
                        return services;
                    }

                    HostLog.Write("GATT service lookup attempt " + attempt + " returned status=" + services.Status +
                        " count=" + services.Services.Count + " connectionStatus=" + device.ConnectionStatus +
                        " sessionMayStillBeSettling=True");
                }
                catch (COMException ex)
                {
                    lastComException = ex;
                }

                await Task.Delay(500);
            }

            if (lastResult != null)
            {
                return lastResult;
            }

            if (lastComException != null)
            {
                throw lastComException;
            }

            throw new InvalidOperationException("GATT service lookup did not produce a result.");
        }

        private static async Task<GattDeviceServicesResult> GetGattServicesForUuidLoggedAsync(BluetoothLEDevice device,
            BluetoothCacheMode cacheMode, string attemptName)
        {
            try
            {
                return await device.GetGattServicesForUuidAsync(CompanionProtocol.ServiceUuid, cacheMode);
            }
            catch (COMException ex)
            {
                HostLog.Write("GATT service lookup " + attemptName + " threw COM exception. HResult=0x" +
                    ex.HResult.ToString("X8") + " cacheMode=" + cacheMode + " connectionStatus=" +
                    device.ConnectionStatus + " isPaired=" + device.DeviceInformation.Pairing.IsPaired, ex);
                throw;
            }
        }

        private async Task UseGattServiceAsync(GattDeviceService service, int generation)
        {
            // Everything is built in locals and only adopted once fully subscribed, so a failed or
            // abandoned attempt never leaves half-initialized state behind.
            var adopted = false;
            GattCharacteristic buttonEvent = null;
            GattCharacteristic participation = null;
            var buttonHandlerAdded = false;
            var participationHandlerAdded = false;
            try
            {
                HostLog.Write("GATT service opened. Uuid=" + service.Uuid);

                var teams = await GetCharacteristicAsync(service, CompanionProtocol.HostTeamsStateUuid, "host teams state");
                var microphone = await GetCharacteristicAsync(service, CompanionProtocol.HostMicrophoneStateUuid, "host microphone state");
                var camera = await GetCharacteristicAsync(service, CompanionProtocol.HostCameraStateUuid, "host camera state");
                var message = await GetCharacteristicAsync(service, CompanionProtocol.HostStatusMessageUuid, "host status message");
                var meeting = await GetCharacteristicAsync(service, CompanionProtocol.HostMeetingStateUuid, "host meeting state");
                var hand = await GetCharacteristicAsync(service, CompanionProtocol.HostHandStateUuid, "host hand state");
                var meetingName = await GetCharacteristicAsync(service, CompanionProtocol.HostMeetingNameUuid, "host meeting name");
                var desktops = await GetCharacteristicAsync(service, CompanionProtocol.HostDesktopStateUuid, "host desktop state");
                var meetingList = await GetCharacteristicAsync(service, CompanionProtocol.HostMeetingListUuid, "host meeting list");
                buttonEvent = await GetCharacteristicAsync(service, CompanionProtocol.ButtonEventUuid, "button event");
                participation = await GetCharacteristicAsync(service,
                    CompanionProtocol.ConnectionParticipationUuid, "connection participation");
                var deviceInfo = await GetCharacteristicAsync(service, CompanionProtocol.DeviceInfoUuid, "device info");
                if (!IsCurrentAttempt(generation))
                {
                    return;
                }

                if (teams == null || microphone == null || camera == null || message == null || meeting == null ||
                    meetingName == null || desktops == null || meetingList == null || hand == null ||
                    buttonEvent == null || participation == null || deviceInfo == null)
                {
                    HostLog.Write("Required characteristic missing. teams=" + (teams != null) +
                        " microphone=" + (microphone != null) +
                        " camera=" + (camera != null) +
                        " message=" + (message != null) +
                        " meeting=" + (meeting != null) +
                        " meetingName=" + (meetingName != null) +
                        " desktops=" + (desktops != null) +
                        " meetingList=" + (meetingList != null) +
                        " hand=" + (hand != null) +
                        " button=" + (buttonEvent != null) +
                        " participation=" + (participation != null) +
                        " deviceInfo=" + (deviceInfo != null));
                    PublishStatus(false, "X3 companion service is missing required characteristics.");
                    ResetGattState();
                    RestartAdvertisementWatcherIfNeeded("missing characteristics");
                    return;
                }

                var protocolVersion = await ReadProtocolVersionAsync(deviceInfo);
                if (!IsCurrentAttempt(generation))
                {
                    return;
                }

                if (protocolVersion != CompanionProtocol.ProtocolVersion)
                {
                    HostLog.Write("Protocol version mismatch. expected=" + CompanionProtocol.ProtocolVersion +
                        " actual=" + (protocolVersion.HasValue ? protocolVersion.Value.ToString() : "unreadable"));
                    PublishStatus(false, "X3 companion protocol version mismatch.");
                    ResetGattState();
                    RestartAdvertisementWatcherIfNeeded("protocol mismatch");
                    return;
                }

                HostLog.Write("Characteristic properties. teams=" + teams.CharacteristicProperties +
                    " microphone=" + microphone.CharacteristicProperties +
                    " camera=" + camera.CharacteristicProperties +
                    " message=" + message.CharacteristicProperties +
                    " meeting=" + meeting.CharacteristicProperties +
                    " meetingName=" + meetingName.CharacteristicProperties +
                    " desktops=" + desktops.CharacteristicProperties +
                    " hand=" + hand.CharacteristicProperties +
                    " button=" + buttonEvent.CharacteristicProperties +
                    " participation=" + participation.CharacteristicProperties +
                    " deviceInfo=" + deviceInfo.CharacteristicProperties +
                    " protocolVersion=" + protocolVersion.Value);

                buttonEvent.ValueChanged += OnButtonEventValueChanged;
                buttonHandlerAdded = true;
                var status = await buttonEvent.WriteClientCharacteristicConfigurationDescriptorAsync(
                    GattClientCharacteristicConfigurationDescriptorValue.Notify);
                HostLog.Write("Button event notification subscribe status=" + status);
                if (!IsCurrentAttempt(generation))
                {
                    return;
                }

                if (status != GattCommunicationStatus.Success)
                {
                    PublishStatus(false, "Could not subscribe to X3 button notifications.");
                    ResetGattState();
                    RestartAdvertisementWatcherIfNeeded("button subscribe failed");
                    return;
                }

                participation.ValueChanged += OnParticipationValueChanged;
                participationHandlerAdded = true;
                var participationStatus =
                    await participation.WriteClientCharacteristicConfigurationDescriptorAsync(
                        GattClientCharacteristicConfigurationDescriptorValue.Notify);
                HostLog.Write("Participation notification subscribe status=" + participationStatus);
                if (!IsCurrentAttempt(generation))
                {
                    return;
                }

                if (participationStatus != GattCommunicationStatus.Success)
                {
                    PublishStatus(false, "Could not subscribe to X3 sync notifications.");
                    ResetGattState();
                    RestartAdvertisementWatcherIfNeeded("participation subscribe failed");
                    return;
                }

                if (buttonEventCharacteristic != null)
                {
                    buttonEventCharacteristic.ValueChanged -= OnButtonEventValueChanged;
                }

                if (participationCharacteristic != null)
                {
                    participationCharacteristic.ValueChanged -= OnParticipationValueChanged;
                }

                companionService?.Dispose();
                companionService = service;
                hostTeamsStateCharacteristic = teams;
                hostMicrophoneStateCharacteristic = microphone;
                hostCameraStateCharacteristic = camera;
                hostStatusMessageCharacteristic = message;
                hostMeetingStateCharacteristic = meeting;
                hostHandStateCharacteristic = hand;
                hostMeetingNameCharacteristic = meetingName;
                hostDesktopStateCharacteristic = desktops;
                hostMeetingListCharacteristic = meetingList;
                buttonEventCharacteristic = buttonEvent;
                participationCharacteristic = participation;
                deviceInfoCharacteristic = deviceInfo;
                adopted = true;
                PublishStatus(true, "Connected to X3 companion.");
            }
            finally
            {
                if (!adopted)
                {
                    if (buttonHandlerAdded)
                    {
                        buttonEvent.ValueChanged -= OnButtonEventValueChanged;
                    }

                    if (participationHandlerAdded)
                    {
                        participation.ValueChanged -= OnParticipationValueChanged;
                    }

                    service.Dispose();
                    if (!IsCurrentAttempt(generation))
                    {
                        HostLog.Write("Abandoned GATT service setup discarded. generation=" + generation);
                    }
                }
            }
        }

        private bool TryBeginConnect(out int generation)
        {
            generation = 0;
            if (disposed || Interlocked.Exchange(ref connecting, 1) == 1)
            {
                return false;
            }

            Interlocked.Exchange(ref connectStartedTicks, DateTime.UtcNow.Ticks);
            generation = Interlocked.Increment(ref connectGeneration);
            return true;
        }

        private bool IsCurrentAttempt(int generation)
        {
            return !disposed && Volatile.Read(ref connectGeneration) == generation;
        }

        private void OnScanWatchdogTick(object state)
        {
            if (disposed)
            {
                return;
            }

            if (connecting != 0)
            {
                // WinRT BLE calls can hang forever (radio off, device rebooted mid-handshake). Without
                // this the connecting flag stays set and every scan/retry path stays disabled.
                var startedUtc = new DateTime(Interlocked.Read(ref connectStartedTicks), DateTimeKind.Utc);
                if (DateTime.UtcNow - startedUtc < ConnectAttemptTimeout)
                {
                    return;
                }

                var abandoned = Interlocked.Increment(ref connectGeneration) - 1;
                HostLog.Write("BLE connect attempt did not finish within " + ConnectAttemptTimeout.TotalSeconds +
                    " s; abandoning it. generation=" + abandoned);
                ResetGattState();
                Interlocked.Exchange(ref connecting, 0);
                PublishStatus(false, "BLE connect attempt timed out; scanning again.");
                EnsureScanningActive("connect timeout");
                return;
            }

            if (IsConnected())
            {
                return;
            }

            EnsureScanningActive("watchdog");
        }

        private static async Task<GattCharacteristic> GetCharacteristicAsync(GattDeviceService service, Guid uuid, string name)
        {
            return await GetCharacteristicAsync(service, uuid, name, 5);
        }

        private static async Task<byte?> ReadProtocolVersionAsync(GattCharacteristic characteristic)
        {
            if (characteristic == null)
            {
                return null;
            }

            try
            {
                var result = await characteristic.ReadValueAsync(BluetoothCacheMode.Uncached);
                if (result.Status != GattCommunicationStatus.Success || result.Value == null ||
                    result.Value.Length < 1)
                {
                    HostLog.Write("Device info read failed. status=" + result.Status +
                        " bytes=" + (result.Value == null ? 0 : result.Value.Length));
                    return null;
                }

                var reader = DataReader.FromBuffer(result.Value);
                return reader.ReadByte();
            }
            catch (Exception ex)
            {
                HostLog.Write("Device info read failed with exception.", ex);
                return null;
            }
        }

        private static async Task<GattCharacteristic> GetCharacteristicAsync(GattDeviceService service, Guid uuid,
            string name, int maxAttempts)
        {
            for (var attempt = 1; attempt <= maxAttempts; attempt++)
            {
                var cacheMode = attempt == 1 ? BluetoothCacheMode.Uncached : BluetoothCacheMode.Cached;
                try
                {
                    var result = await service.GetCharacteristicsForUuidAsync(uuid, cacheMode);
                    HostLog.Write("Characteristic lookup " + name + " attempt=" + attempt + " uuid=" + uuid +
                        " cacheMode=" + cacheMode + " status=" + result.Status + " count=" +
                        result.Characteristics.Count + " serviceUuid=" + service.Uuid);
                    if (result.Status == GattCommunicationStatus.Success && result.Characteristics.Count > 0)
                    {
                        return result.Characteristics[0];
                    }
                }
                catch (COMException ex)
                {
                    HostLog.Write("Characteristic lookup " + name + " attempt=" + attempt +
                        " threw COM exception. HResult=0x" + ex.HResult.ToString("X8") + " uuid=" + uuid +
                        " cacheMode=" + cacheMode + " serviceUuid=" + service.Uuid, ex);
                }
                catch (OperationCanceledException ex)
                {
                    HostLog.Write("Characteristic lookup " + name + " attempt=" + attempt +
                        " was canceled. uuid=" + uuid + " cacheMode=" + cacheMode + " serviceUuid=" +
                        service.Uuid, ex);
                }

                await Task.Delay(500);
            }

            return null;
        }

        private void ResetGattState()
        {
            if (buttonEventCharacteristic != null)
            {
                buttonEventCharacteristic.ValueChanged -= OnButtonEventValueChanged;
                buttonEventCharacteristic = null;
            }
            if (participationCharacteristic != null)
            {
                participationCharacteristic.ValueChanged -= OnParticipationValueChanged;
                participationCharacteristic = null;
            }

            deviceInfoCharacteristic = null;
            participationCharacteristic = null;
            lastParticipationCounter = 0;
            lastParticipationReceivedAt = null;
            hostTeamsStateCharacteristic = null;
            hostMicrophoneStateCharacteristic = null;
            hostCameraStateCharacteristic = null;
            hostStatusMessageCharacteristic = null;
            hostMeetingStateCharacteristic = null;
            hostHandStateCharacteristic = null;
            hostMeetingNameCharacteristic = null;
            hostDesktopStateCharacteristic = null;
            hostMeetingListCharacteristic = null;
            companionService?.Dispose();
            companionService = null;
            if (gattSession != null)
            {
                gattSession.SessionStatusChanged -= OnGattSessionStatusChanged;
                gattSession.Dispose();
                gattSession = null;
            }

            companionDevice?.Dispose();
            companionDevice = null;
        }

        private void LogAdvertisement(BluetoothLEAdvertisementReceivedEventArgs args)
        {
            var now = DateTimeOffset.Now;
            if (advertisementLogTimes.TryGetValue(args.BluetoothAddress, out var previous) &&
                now - previous < TimeSpan.FromSeconds(5))
            {
                return;
            }

            advertisementLogTimes[args.BluetoothAddress] = now;
            HostLog.Write("BLE advertisement received. Address=" + FormatBluetoothAddress(args.BluetoothAddress) +
                " Name=" + args.Advertisement.LocalName + " RSSI=" + args.RawSignalStrengthInDBm +
                " AddressType=" + args.BluetoothAddressType + " Type=" + args.AdvertisementType + " ServiceUuids=" +
                string.Join(",", args.Advertisement.ServiceUuids));
        }

        private void RestartAdvertisementWatcherIfNeeded(string reason)
        {
            EnsureScanningActive(reason);
            _ = RetryScanningAfterDelayAsync(reason);
        }

        private async Task RetryScanningAfterDelayAsync(string reason)
        {
            await Task.Delay(1000);
            EnsureScanningActive(reason + " retry 1s");
            await Task.Delay(4000);
            EnsureScanningActive(reason + " retry 5s");
        }

        private void EnsureScanningActive(string reason)
        {
            try
            {
                if (watcher != null &&
                    watcher.Status != DeviceWatcherStatus.Started &&
                    watcher.Status != DeviceWatcherStatus.EnumerationCompleted &&
                    watcher.Status != DeviceWatcherStatus.Stopping)
                {
                    watcher.Start();
                    HostLog.Write("BLE device watcher restarted by " + reason + ". Status=" + watcher.Status);
                }
            }
            catch (Exception ex)
            {
                HostLog.Write("BLE device watcher restart failed from " + reason + ".", ex);
            }

            try
            {
                if (advertisementWatcher != null &&
                    advertisementWatcher.Status != BluetoothLEAdvertisementWatcherStatus.Started &&
                    advertisementWatcher.Status != BluetoothLEAdvertisementWatcherStatus.Stopping)
                {
                    intentionallyPausedAdvertisementWatcher = false;
                    advertisementWatcher.Start();
                    HostLog.Write("BLE advertisement watcher restarted by " + reason + ". Status=" +
                        advertisementWatcher.Status);
                }
            }
            catch (Exception ex)
            {
                HostLog.Write("BLE advertisement watcher restart failed from " + reason + ".", ex);
            }
        }

        private static string FormatBluetoothAddress(ulong address)
        {
            return string.Format("{0:X2}:{1:X2}:{2:X2}:{3:X2}:{4:X2}:{5:X2}",
                (address >> 40) & 0xFF,
                (address >> 32) & 0xFF,
                (address >> 24) & 0xFF,
                (address >> 16) & 0xFF,
                (address >> 8) & 0xFF,
                address & 0xFF);
        }

        private void OnButtonEventValueChanged(GattCharacteristic sender, GattValueChangedEventArgs args)
        {
            var reader = DataReader.FromBuffer(args.CharacteristicValue);
            reader.ByteOrder = ByteOrder.LittleEndian;
            if (reader.UnconsumedBufferLength < CompanionProtocol.ButtonEventPayloadLength)
            {
                HostLog.Write("Button event notification ignored; payload too short.");
                return;
            }

            var version = reader.ReadByte();
            var button = reader.ReadByte();
            var action = reader.ReadByte();
            var sequence = reader.ReadUInt16();
            var uptimeMs = reader.ReadUInt32();
            var argument = reader.ReadByte();
            var meetingRevision = reader.ReadUInt16();

            if (version != CompanionProtocol.ProtocolVersion)
            {
                HostLog.Write("Button event ignored. version=" + version +
                    " button=" + button + " action=" + action + " seq=" + sequence);
                return;
            }

            if (!Enum.IsDefined(typeof(CompanionButton), button) ||
                action != (byte)CompanionButtonAction.Released)
            {
                HostLog.Write("Button event ignored. button=" + button + " action=" + action +
                    " seq=" + sequence + " uptimeMs=" + uptimeMs);
                return;
            }

            HostLog.Write("Button event received. seq=" + sequence + " button=" + button +
                " action=" + action + " argument=" + argument + " uptimeMs=" + uptimeMs +
                " hostReceivedAt=" + DateTimeOffset.Now);
            ButtonEventReceived?.Invoke(this, new CompanionButtonEvent((CompanionButton)button,
                (CompanionButtonAction)action, sequence, uptimeMs, argument, meetingRevision));
        }

        private void OnParticipationValueChanged(GattCharacteristic sender, GattValueChangedEventArgs args)
        {
            var reader = DataReader.FromBuffer(args.CharacteristicValue);
            reader.ByteOrder = ByteOrder.LittleEndian;
            if (reader.UnconsumedBufferLength < 5)
            {
                HostLog.Write("Participation notification ignored; payload too short.");
                return;
            }

            var version = reader.ReadByte();
            var counter = reader.ReadUInt32();
            var receivedAt = DateTimeOffset.Now;
            if (version != CompanionProtocol.ProtocolVersion)
            {
                HostLog.Write("Participation notification ignored. version=" + version + " counter=" + counter);
                return;
            }

            var deltaCounter = lastParticipationCounter == 0 ? 0 : counter - lastParticipationCounter;
            var elapsedMs = lastParticipationReceivedAt.HasValue
                ? (receivedAt - lastParticipationReceivedAt.Value).TotalMilliseconds
                : 0;
            lastParticipationCounter = counter;
            lastParticipationReceivedAt = receivedAt;
            HostLog.Write("Participation notification received. counter=" + counter +
                " deltaCounter=" + deltaCounter + " elapsedMs=" + elapsedMs.ToString("F0") +
                " hostReceivedAt=" + receivedAt);
            ParticipationEventReceived?.Invoke(this, new CompanionParticipationEvent(counter));
        }

        private void PublishStatus(bool connected, string message)
        {
            HostLog.Write("Status changed. connected=" + connected + " message=" + message);
            StatusChanged?.Invoke(this, new CompanionConnectionStatus(connected, message));
        }

        private bool IsConnected()
        {
            return companionService != null &&
                hostTeamsStateCharacteristic != null &&
                hostMicrophoneStateCharacteristic != null &&
                hostCameraStateCharacteristic != null &&
                hostStatusMessageCharacteristic != null &&
                buttonEventCharacteristic != null;
        }
    }
}
