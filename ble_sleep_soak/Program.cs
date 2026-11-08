// BLE soak test for X4 Pro auto light sleep.
//
// Connects to the companion device like laptop_host does (GattSession with
// MaintainConnection), completes the minimal handshake so the device drops to
// its idle connection parameters, then periodically reads the debug sleep
// diagnostics characteristic (key=value text, see src/BtSleepTiming.h and
// CompanionBleService::publishSleepDiagnostics) and logs disconnects.
//
// It can also drive A/B experiments by writing commands to the same
// characteristic, e.g. alternating the BT wake auto-tuner every N minutes.
//
//   dotnet run --project ble_sleep_soak -- --minutes 60 --set "pm=1 als=1"
//   dotnet run --project ble_sleep_soak -- --minutes 80 --set "pm=1 als=1" --ab tune --phase-minutes 20
//   dotnet run --project ble_sleep_soak -- --no-handshake   (laptop_host already connected)

using System.Globalization;
using System.Runtime.InteropServices.WindowsRuntime;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Storage.Streams;

internal static class Program
{
    private static readonly Guid ServiceUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000001");
    private static readonly Guid TeamsStateUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000002");
    private static readonly Guid ButtonEventUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000006");
    private static readonly Guid SleepDiagnosticsUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a0000f0");

    private sealed class Options
    {
        public double Minutes = 30;
        public int PollSeconds = 30;
        public string? Set;
        public string? AbKey;
        public double PhaseMinutes = 15;
        public bool Handshake = true;
        public ulong? Address;
        public string LogPath = $"ble_sleep_soak_{DateTime.Now:yyyyMMdd_HHmmss}.log";
    }

    private sealed class PhaseSummary
    {
        public string Name = "";
        public int Disconnects;
        public int ReadFailures;
        public Dictionary<string, string>? Last;
    }

    private static BluetoothLEDevice? device;
    private static GattSession? session;
    private static GattCharacteristic? diagnostics;
    private static GattCharacteristic? buttonEvents;
    private static StreamWriter? log;
    private static PhaseSummary currentPhase = new() { Name = "all" };
    private static readonly List<PhaseSummary> Phases = new();
    private static bool doHandshake = true;

    private static async Task<int> Main(string[] args)
    {
        var options = Parse(args);
        if (options == null) return 2;

        log = new StreamWriter(options.LogPath, append: false) { AutoFlush = true };
        Log($"# BleSleepSoak minutes={options.Minutes} poll={options.PollSeconds}s set='{options.Set}' " +
            $"ab={options.AbKey} phase={options.PhaseMinutes}m handshake={options.Handshake}");

        // A device Windows is still connected to does not advertise; --address attaches to it directly.
        var address = options.Address.HasValue
            ? (options.Address.Value, BluetoothAddressType.Public)
            : await FindDeviceAsync(TimeSpan.FromSeconds(60));
        if (address == null)
        {
            Log("! No advertising companion device found (is it already connected to laptop_host? use --no-handshake " +
                "after pairing, or stop laptop_host).");
            return 1;
        }

        if (!await ConnectAsync(address.Value.Address, address.Value.Type, options.Handshake)) return 1;

        // Counters are reset only when an experiment starts, so a recovery command
        // (e.g. --set "als=0") leaves the failing window's statistics readable.
        if (!string.IsNullOrWhiteSpace(options.Set))
            await SendCommandAsync(options.Set.Contains("als=1") ? options.Set + " reset=1" : options.Set);

        var start = DateTime.Now;
        var deadline = start.AddMinutes(options.Minutes);
        var phaseIndex = -1;
        while (DateTime.Now < deadline)
        {
            if (options.AbKey != null)
            {
                var index = (int)((DateTime.Now - start).TotalMinutes / options.PhaseMinutes);
                if (index != phaseIndex)
                {
                    phaseIndex = index;
                    var value = index % 2;
                    StartPhase($"{options.AbKey}={value}#{index}");
                    await SendCommandAsync($"{options.AbKey}={value} reset=1");
                }
            }
            else if (phaseIndex < 0)
            {
                phaseIndex = 0;
                StartPhase("run");
            }

            await Task.Delay(TimeSpan.FromSeconds(options.PollSeconds));
            await PollAsync();
        }

        Phases.Add(currentPhase);
        Log("# Summary (per phase; device counters are reset at each phase start)");
        foreach (var phase in Phases.Where(p => p.Last != null || p.Disconnects > 0))
        {
            var s = phase.Last ?? new Dictionary<string, string>();
            Log($"# {phase.Name,-12} disconnects={phase.Disconnects} read_failures={phase.ReadFailures} " +
                $"sleep={Pct(s, "sleep_pm")} bt_awake={Pct(s, "bt_pm")} wakes={Get(s, "wakes")} " +
                $"late={Get(s, "late")} miss={Get(s, "miss")} min_us={Get(s, "min_us")} avg_us={Get(s, "avg_us")} " +
                $"lead_us={Get(s, "lead_us")}/{Get(s, "idf_lead_us")} cal_ppm={Get(s, "cal_min")}..{Get(s, "cal_max")} " +
                $"sto={Get(s, "sto")} gap_disc={Get(s, "gap_disc")}");
        }
        session?.Dispose();
        device?.Dispose();
        return 0;
    }

    private static void StartPhase(string name)
    {
        if (currentPhase.Last != null || currentPhase.Disconnects > 0) Phases.Add(currentPhase);
        currentPhase = new PhaseSummary { Name = name };
        Log($"# phase {name}");
    }

    private static async Task PollAsync()
    {
        if (diagnostics == null)
        {
            currentPhase.ReadFailures++;
            Log("! diagnostics characteristic unavailable");
            await TryRefreshCharacteristicsAsync();
            return;
        }

        try
        {
            var result = await diagnostics.ReadValueAsync(BluetoothCacheMode.Uncached);
            if (result.Status != GattCommunicationStatus.Success)
            {
                currentPhase.ReadFailures++;
                Log($"! read failed: {result.Status}");
                return;
            }

            // Keepalive for the device's light-sleep dead-man switch: if this tool
            // stops reaching the device, BLE-enabled light sleep reverts by itself.
            await SendKeepaliveAsync();
            var text = Encoding(result.Value);
            var kv = ParseKv(text);
            currentPhase.Last = kv;
            log?.WriteLine($"{DateTime.Now:O} phase={currentPhase.Name} {text}");
            Console.WriteLine(
                $"{DateTime.Now:HH:mm:ss} {device?.ConnectionStatus} sleep={Pct(kv, "sleep_pm")} bt={Pct(kv, "bt_pm")} " +
                $"itvl={Get(kv, "itvl")}/lat{Get(kv, "lat")} wakes={Get(kv, "wakes")} late={Get(kv, "late")} " +
                $"miss={Get(kv, "miss")} min={Get(kv, "min_us")}us avg={Get(kv, "avg_us")}us " +
                $"lead={Get(kv, "lead_us")}/{Get(kv, "idf_lead_us")}us tune={Get(kv, "tune")} " +
                $"cal={Get(kv, "cal_min")}..{Get(kv, "cal_max")}ppm rtcf={Get(kv, "rtcf")} " +
                $"disc={Get(kv, "gap_disc")} sto={Get(kv, "sto")} reason={Get(kv, "disc_reason")}");
        }
        catch (Exception ex)
        {
            currentPhase.ReadFailures++;
            Log($"! read exception: {ex.GetType().Name} 0x{ex.HResult:X8} {ex.Message}");
            await TryRefreshCharacteristicsAsync();
        }
    }

    private static async Task SendCommandAsync(string command)
    {
        if (diagnostics == null) return;
        var writer = new DataWriter();
        writer.WriteString(command);
        var status = await diagnostics.WriteValueWithResultAsync(writer.DetachBuffer(), GattWriteOption.WriteWithResponse);
        Log($"> {command} ({status.Status})");
        // The device applies commands from its worker loop (<= 0.5 s).
        await Task.Delay(1500);
    }

    private static async Task SendKeepaliveAsync()
    {
        if (diagnostics == null) return;
        try
        {
            var writer = new DataWriter();
            writer.WriteString("ka=1");
            await diagnostics.WriteValueWithResultAsync(writer.DetachBuffer(), GattWriteOption.WriteWithResponse);
        }
        catch (Exception ex)
        {
            Log($"! keepalive failed: {ex.GetType().Name} 0x{ex.HResult:X8}");
        }
    }

    private static async Task<(ulong Address, BluetoothAddressType Type)?> FindDeviceAsync(TimeSpan timeout)
    {
        var found = new TaskCompletionSource<(ulong, BluetoothAddressType)>(TaskCreationOptions.RunContinuationsAsynchronously);
        var watcher = new BluetoothLEAdvertisementWatcher { ScanningMode = BluetoothLEScanningMode.Active };
        watcher.AdvertisementFilter.Advertisement.ServiceUuids.Add(ServiceUuid);
        watcher.Received += (_, a) =>
        {
            Log($"# found {a.Advertisement.LocalName} {a.BluetoothAddress:X12} rssi={a.RawSignalStrengthInDBm}");
            found.TrySetResult((a.BluetoothAddress, a.BluetoothAddressType));
        };
        Log("# scanning for companion service...");
        watcher.Start();
        var winner = await Task.WhenAny(found.Task, Task.Delay(timeout));
        watcher.Stop();
        return winner == found.Task ? found.Task.Result : ((ulong, BluetoothAddressType)?)null;
    }

    private static async Task<bool> ConnectAsync(ulong address, BluetoothAddressType addressType, bool handshake)
    {
        device = await BluetoothLEDevice.FromBluetoothAddressAsync(address, addressType);
        if (device == null)
        {
            Log("! FromBluetoothAddressAsync returned null");
            return false;
        }

        device.ConnectionStatusChanged += (d, _) =>
        {
            if (d.ConnectionStatus == BluetoothConnectionStatus.Disconnected) currentPhase.Disconnects++;
            Log($"# connection {d.ConnectionStatus}");
            if (d.ConnectionStatus == BluetoothConnectionStatus.Connected) _ = ReconnectAsync();
        };

        session = await GattSession.FromDeviceIdAsync(device.BluetoothDeviceId);
        if (session.CanMaintainConnection)
        {
            session.MaintainConnection = true;
            await Task.Delay(250);
        }

        doHandshake = handshake;
        if (!await TryRefreshCharacteristicsAsync()) return false;
        if (handshake) await HandshakeAsync();
        return true;
    }

    // Windows re-establishes the maintained session by itself, but the device
    // drops hosts that do not finish the handshake within 15 s, so redo it.
    private static async Task ReconnectAsync()
    {
        if (await TryRefreshCharacteristicsAsync() && doHandshake) await HandshakeAsync();
    }

    private static readonly SemaphoreSlim RefreshLock = new(1, 1);

    private static async Task<bool> TryRefreshCharacteristicsAsync()
    {
        if (device == null) return false;
        await RefreshLock.WaitAsync();
        try
        {
            for (var attempt = 1; attempt <= 10; attempt++)
            {
                try
                {
                    // Alternate cached/uncached: a cached lookup succeeds immediately on a quick
                    // reconnect (when the link is flapping), uncached handles a changed GATT table.
                    var cacheMode = attempt % 2 == 1 ? BluetoothCacheMode.Cached : BluetoothCacheMode.Uncached;
                    var services = await device.GetGattServicesForUuidAsync(ServiceUuid, cacheMode);
                    if (services.Status != GattCommunicationStatus.Success || services.Services.Count == 0)
                    {
                        Log($"! service lookup failed: {services.Status} (attempt {attempt})");
                    }
                    else
                    {
                        var service = services.Services[0];
                        diagnostics = await FindCharacteristicAsync(service, SleepDiagnosticsUuid);
                        buttonEvents = await FindCharacteristicAsync(service, ButtonEventUuid);
                        if (diagnostics == null) Log("! device firmware has no sleep diagnostics characteristic (...0000f0)");
                        return diagnostics != null;
                    }
                }
                catch (Exception ex)
                {
                    Log($"! characteristic refresh failed (attempt {attempt}): {ex.GetType().Name} 0x{ex.HResult:X8} {ex.Message}");
                }
                await Task.Delay(TimeSpan.FromSeconds(2));
            }
            return false;
        }
        finally
        {
            RefreshLock.Release();
        }
    }

    private static async Task<GattCharacteristic?> FindCharacteristicAsync(GattDeviceService service, Guid uuid)
    {
        var result = await service.GetCharacteristicsForUuidAsync(uuid, BluetoothCacheMode.Uncached);
        return result.Status == GattCommunicationStatus.Success && result.Characteristics.Count > 0
            ? result.Characteristics[0]
            : null;
    }

    // Mirrors the parts of laptop_host's handshake that gate the device's idle
    // connection parameters: button-event subscription plus one host state write.
    private static async Task HandshakeAsync()
    {
        if (buttonEvents != null)
        {
            buttonEvents.ValueChanged += (_, e) => Log($"# button event {BitConverter.ToString(e.CharacteristicValue.ToArray())}");
            var status = await buttonEvents.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue.Notify);
            Log($"# button events subscribe {status}");
        }

        var service = buttonEvents?.Service;
        if (service == null) return;
        var teams = await FindCharacteristicAsync(service, TeamsStateUuid);
        if (teams == null) return;
        // CompanionProtocol::encodeState(on=false, counter=1) == 0x0004, little endian.
        var writer = new DataWriter();
        writer.WriteBytes(new byte[] { 0x04, 0x00 });
        var write = await teams.WriteValueWithResultAsync(writer.DetachBuffer(), GattWriteOption.WriteWithResponse);
        Log($"# teams state write {write.Status}");
    }

    private static string Encoding(IBuffer buffer) =>
        System.Text.Encoding.UTF8.GetString(buffer.ToArray()).TrimEnd('\0', ' ', '\n');

    private static Dictionary<string, string> ParseKv(string text)
    {
        var kv = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var token in text.Split(' ', StringSplitOptions.RemoveEmptyEntries))
        {
            var eq = token.IndexOf('=');
            if (eq > 0) kv[token[..eq]] = token[(eq + 1)..];
        }
        return kv;
    }

    private static string Get(Dictionary<string, string> kv, string key) => kv.TryGetValue(key, out var v) ? v : "?";

    private static string Pct(Dictionary<string, string> kv, string permilleKey) =>
        kv.TryGetValue(permilleKey, out var v) && int.TryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out var p)
            ? $"{p / 10}.{p % 10}%"
            : "?";

    private static void Log(string line)
    {
        var stamped = $"{DateTime.Now:HH:mm:ss} {line}";
        Console.WriteLine(stamped);
        log?.WriteLine(stamped);
    }

    private static Options? Parse(string[] args)
    {
        var o = new Options();
        for (var i = 0; i < args.Length; i++)
        {
            string Next() => i + 1 < args.Length ? args[++i] : throw new ArgumentException($"{args[i]} needs a value");
            switch (args[i])
            {
                case "--minutes": o.Minutes = double.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--poll": o.PollSeconds = int.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--set": o.Set = Next(); break;
                case "--ab": o.AbKey = Next(); break;
                case "--phase-minutes": o.PhaseMinutes = double.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--no-handshake": o.Handshake = false; break;
                case "--log": o.LogPath = Next(); break;
                case "--address": o.Address = ulong.Parse(Next().Replace(":", ""), NumberStyles.HexNumber, CultureInfo.InvariantCulture); break;
                default:
                    Console.WriteLine("usage: BleSleepSoak [--minutes N] [--poll S] [--set \"pm=1 als=1 tune=0 rtcf=0\"]");
                    Console.WriteLine("                    [--ab tune|rtcf|als --phase-minutes M] [--no-handshake] [--log FILE] [--address 7CE8B1AB5641]");
                    return null;
            }
        }
        return o;
    }
}
