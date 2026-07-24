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
        private GattCharacteristic buttonEventCharacteristic;
        private GattCharacteristic participationCharacteristic;
        private GattCharacteristic deviceInfoCharacteristic;
        private readonly SemaphoreSlim hostStateWriteLock = new SemaphoreSlim(1, 1);
        private Timer scanWatchdogTimer;
        private int connecting;
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
            ushort cameraCounter = 0, ushort handCounter = 0)
        {
            if (!await hostStateWriteLock.WaitAsync(0))
            {
                HostLog.Write("Host state skipped; previous BLE write is still pending.");
                return false;
            }

            try
            {
                if (hostTeamsStateCharacteristic == null || hostMicrophoneStateCharacteristic == null ||
                    hostCameraStateCharacteristic == null || hostStatusMessageCharacteristic == null)
                {
                    HostLog.Write("Host state skipped; one or more state characteristics are not available.");
                    return false;
                }

                var startedAt = DateTimeOffset.Now;
                HostLog.Write("Host state write starting option=WriteWithoutResponse teams=" + teamsDetected +
                    " meeting=" + meetingDetected + " meetingName=" + meetingName +
                    " mic=" + microphone + " camera=" + camera +
                    " hand=" + hand + " counters=" + teamsCounter + "/" + meetingCounter + "/" +
                    microphoneCounter + "/" + cameraCounter + "/" + handCounter + " message=" + message);
                var teamsStatus = await WriteEncodedStateAsync(hostTeamsStateCharacteristic, teamsDetected,
                    teamsCounter, "teams");
                var meetingStatus = await WriteOptionalByteStateAsync(hostMeetingStateCharacteristic,
                    meetingDetected, meetingCounter, "meeting");
                var meetingNameStatus = await WriteOptionalStringStateAsync(hostMeetingNameCharacteristic,
                    meetingName, "meeting name");
                var microphoneStatus = await WriteEncodedStateAsync(hostMicrophoneStateCharacteristic,
                    CompanionProtocol.TriStateIsOn(microphone), microphoneCounter, "microphone");
                var cameraStatus = await WriteEncodedStateAsync(hostCameraStateCharacteristic,
                    CompanionProtocol.TriStateIsOn(camera), cameraCounter, "camera");
                var handStatus = await WriteOptionalByteStateAsync(hostHandStateCharacteristic,
                    CompanionProtocol.TriStateIsOn(hand), handCounter, "hand");
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
            bool on, ushort counter, string name)
        {
            var writer = new DataWriter
            {
                ByteOrder = ByteOrder.LittleEndian
            };
            var encoded = CompanionProtocol.EncodeState(on, counter);
            writer.WriteUInt16(encoded);
            var buffer = writer.DetachBuffer();
            HostLog.Write("Host state write " + name + " bytes=" + buffer.Length +
                " properties=" + characteristic.CharacteristicProperties);
            var status = await characteristic.WriteValueAsync(buffer, GattWriteOption.WriteWithoutResponse);
            HostLog.Write("Host state write " + name + " status=" + status + " on=" + on +
                " counter=" + counter + " encoded=" + encoded);
            return status;
        }

        private static async Task<GattCommunicationStatus> WriteOptionalByteStateAsync(GattCharacteristic characteristic,
            bool on, ushort counter, string name)
        {
            if (characteristic == null)
            {
                HostLog.Write("Host state write " + name + " skipped; optional characteristic is missing.");
                return GattCommunicationStatus.Success;
            }

            return await WriteEncodedStateAsync(characteristic, on, counter, name);
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

        private static async Task<GattCommunicationStatus> WriteOptionalStringStateAsync(GattCharacteristic characteristic,
            string value, string name)
        {
            if (characteristic == null)
            {
                HostLog.Write("Host state write " + name + " skipped; optional characteristic is missing.");
                return GattCommunicationStatus.Success;
            }

            return await WriteStringStateAsync(characteristic, value, name);
        }

        private async void OnDeviceAdded(DeviceWatcher sender, DeviceInformation args)
        {
            HostLog.Write("BLE device added. Name=" + args.Name + " IsPaired=" + args.Pairing.IsPaired + " Id=" + args.Id);
            if (IsConnected())
            {
                HostLog.Write("BLE device add ignored; already connected.");
                return;
            }

            if (disposed || Interlocked.Exchange(ref connecting, 1) == 1)
            {
                HostLog.Write("BLE device add ignored. disposed=" + disposed + " connecting=" + connecting);
                return;
            }

            try
            {
                await ConnectAsync(args);
            }
            catch (Exception ex)
            {
                HostLog.Write("BLE connect failed with exception.", ex);
                PublishStatus(false, "BLE connect exception: " + ex.Message);
            }
            finally
            {
                if (!IsConnected())
                {
                    ResetGattState();
                    RestartAdvertisementWatcherIfNeeded("device add not connected");
                }

                Interlocked.Exchange(ref connecting, 0);
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

            if (Interlocked.Exchange(ref connecting, 1) == 1)
            {
                return;
            }

            try
            {
                await ConnectFromAdvertisementAsync(args.BluetoothAddress, args.BluetoothAddressType);
            }
            catch (COMException ex)
            {
                HostLog.Write("BLE advertisement connect failed with COM exception. HResult=0x" +
                    ex.HResult.ToString("X8"), ex);
                PublishStatus(false, "BLE GATT connect failed: 0x" + ex.HResult.ToString("X8"));
            }
            catch (Exception ex)
            {
                HostLog.Write("BLE advertisement connect failed with exception.", ex);
                PublishStatus(false, "BLE advertisement connect exception: " + ex.Message);
            }
            finally
            {
                if (!IsConnected())
                {
                    ResetGattState();
                }

                Interlocked.Exchange(ref connecting, 0);
                RestartAdvertisementWatcherIfNeeded("advertisement connect finally");
            }
        }

        private void OnAdvertisementWatcherStopped(BluetoothLEAdvertisementWatcher sender, BluetoothLEAdvertisementWatcherStoppedEventArgs args)
        {
            HostLog.Write("BLE advertisement watcher stopped. Status=" + sender.Status + " Error=" + args.Error);
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
            HostLog.Write("GattSession status changed. Status=" + args.Status + " Error=" + args.Error);
            if (disposed || connecting != 0 || args.Status != GattSessionStatus.Closed)
            {
                return;
            }

            ResetGattState();
            RestartAdvertisementWatcherIfNeeded("gatt session closed");
            PublishStatus(false, "X3 companion GATT session closed.");
        }

        private async Task ConnectAsync(DeviceInformation deviceInfo)
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

            await UseGattServiceAsync(service);
        }

        private async Task ConnectFromAdvertisementAsync(ulong bluetoothAddress, BluetoothAddressType addressType)
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

            var device = await BluetoothLEDevice.FromBluetoothAddressAsync(bluetoothAddress, addressType);
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

            var session = await CreateMaintainedGattSessionAsync(device);
            var services = await GetGattServicesWithRetryAsync(device);
            HostLog.Write("Advertisement GATT service lookup status=" + services.Status + " count=" + services.Services.Count);
            if (services.Status != GattCommunicationStatus.Success || services.Services.Count == 0)
            {
                session?.Dispose();
                device.Dispose();
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
            await UseGattServiceAsync(services.Services[0]);
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

        private async Task UseGattServiceAsync(GattDeviceService service)
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

            companionService?.Dispose();
            companionService = service;
            HostLog.Write("GATT service opened. Uuid=" + service.Uuid);

            hostTeamsStateCharacteristic = await GetCharacteristicAsync(service, CompanionProtocol.HostTeamsStateUuid, "host teams state");
            hostMicrophoneStateCharacteristic = await GetCharacteristicAsync(service, CompanionProtocol.HostMicrophoneStateUuid, "host microphone state");
            hostCameraStateCharacteristic = await GetCharacteristicAsync(service, CompanionProtocol.HostCameraStateUuid, "host camera state");
            hostStatusMessageCharacteristic = await GetCharacteristicAsync(service, CompanionProtocol.HostStatusMessageUuid, "host status message");
            hostMeetingStateCharacteristic = await GetOptionalCharacteristicAsync(service, CompanionProtocol.HostMeetingStateUuid, "host meeting state");
            hostHandStateCharacteristic = await GetOptionalCharacteristicAsync(service, CompanionProtocol.HostHandStateUuid, "host hand state");
            hostMeetingNameCharacteristic = await GetOptionalCharacteristicAsync(service, CompanionProtocol.HostMeetingNameUuid, "host meeting name");
            buttonEventCharacteristic = await GetCharacteristicAsync(service, CompanionProtocol.ButtonEventUuid, "button event");
            participationCharacteristic = await GetOptionalCharacteristicAsync(service,
                CompanionProtocol.ConnectionParticipationUuid, "connection participation");
            deviceInfoCharacteristic = await GetCharacteristicAsync(service, CompanionProtocol.DeviceInfoUuid, "device info");

            if (hostTeamsStateCharacteristic == null || hostMicrophoneStateCharacteristic == null ||
                hostCameraStateCharacteristic == null || hostStatusMessageCharacteristic == null ||
                buttonEventCharacteristic == null)
            {
                HostLog.Write("Required characteristic missing. teams=" + (hostTeamsStateCharacteristic != null) +
                    " microphone=" + (hostMicrophoneStateCharacteristic != null) +
                    " camera=" + (hostCameraStateCharacteristic != null) +
                    " message=" + (hostStatusMessageCharacteristic != null) +
                    " meeting=" + (hostMeetingStateCharacteristic != null) +
                    " meetingName=" + (hostMeetingNameCharacteristic != null) +
                    " hand=" + (hostHandStateCharacteristic != null) +
                    " button=" + (buttonEventCharacteristic != null) +
                    " participation=" + (participationCharacteristic != null) +
                    " deviceInfo=" + (deviceInfoCharacteristic != null));
                PublishStatus(false, "X3 companion service is missing required characteristics.");
                ResetGattState();
                RestartAdvertisementWatcherIfNeeded("missing characteristics");
                return;
            }

            HostLog.Write("Characteristic properties. teams=" + hostTeamsStateCharacteristic.CharacteristicProperties +
                " microphone=" + hostMicrophoneStateCharacteristic.CharacteristicProperties +
                " camera=" + hostCameraStateCharacteristic.CharacteristicProperties +
                " message=" + hostStatusMessageCharacteristic.CharacteristicProperties +
                " meeting=" + (hostMeetingStateCharacteristic == null ? "missing" : hostMeetingStateCharacteristic.CharacteristicProperties.ToString()) +
                " meetingName=" + (hostMeetingNameCharacteristic == null ? "missing" : hostMeetingNameCharacteristic.CharacteristicProperties.ToString()) +
                " hand=" + (hostHandStateCharacteristic == null ? "missing" : hostHandStateCharacteristic.CharacteristicProperties.ToString()) +
                " button=" + buttonEventCharacteristic.CharacteristicProperties +
                " participation=" +
                (participationCharacteristic == null ? "missing" : participationCharacteristic.CharacteristicProperties.ToString()) +
                " deviceInfo=" +
                (deviceInfoCharacteristic == null ? "missing" : deviceInfoCharacteristic.CharacteristicProperties.ToString()));

            buttonEventCharacteristic.ValueChanged += OnButtonEventValueChanged;
            var status = await buttonEventCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue.Notify);
            HostLog.Write("Button event notification subscribe status=" + status);
            if (status != GattCommunicationStatus.Success)
            {
                PublishStatus(false, "Could not subscribe to X3 button notifications.");
                ResetGattState();
                RestartAdvertisementWatcherIfNeeded("button subscribe failed");
                return;
            }

            if (participationCharacteristic != null)
            {
                participationCharacteristic.ValueChanged += OnParticipationValueChanged;
                var participationStatus =
                    await participationCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
                        GattClientCharacteristicConfigurationDescriptorValue.Notify);
                HostLog.Write("Participation notification subscribe status=" + participationStatus);
            }

            PublishStatus(true, "Connected to X3 companion.");
        }

        private void OnScanWatchdogTick(object state)
        {
            if (disposed || IsConnected() || connecting != 0)
            {
                return;
            }

            EnsureScanningActive("watchdog");
        }

        private static async Task<GattCharacteristic> GetCharacteristicAsync(GattDeviceService service, Guid uuid, string name)
        {
            return await GetCharacteristicAsync(service, uuid, name, 5);
        }

        private static async Task<GattCharacteristic> GetOptionalCharacteristicAsync(GattDeviceService service,
            Guid uuid, string name)
        {
            return await GetCharacteristicAsync(service, uuid, name, 1);
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
            if (reader.UnconsumedBufferLength < 9)
            {
                HostLog.Write("Button event notification ignored; payload too short.");
                return;
            }

            var version = reader.ReadByte();
            var button = reader.ReadByte();
            var action = reader.ReadByte();
            var sequence = reader.ReadUInt16();
            var uptimeMs = reader.ReadUInt32();

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
                " action=" + action + " uptimeMs=" + uptimeMs + " hostReceivedAt=" + DateTimeOffset.Now);
            ButtonEventReceived?.Invoke(this, new CompanionButtonEvent((CompanionButton)button,
                (CompanionButtonAction)action, sequence, uptimeMs));
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
