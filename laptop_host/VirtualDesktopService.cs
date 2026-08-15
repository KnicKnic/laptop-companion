using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32;

namespace X3LaptopCompanion
{
    /// <summary>
    /// Enumerates Windows virtual desktops (including a Windows App / Windows 365 remote
    /// session that has been added to Task view) and switches between them.
    /// </summary>
    /// <remarks>
    /// Desktop discovery prefers the shell's internal virtual desktop manager because it is the
    /// only source that lists remote desktops; the registry is used as a fallback and to tell
    /// local desktops apart from remote ones (remote desktops never appear in the registry list).
    /// Only the interface slots that have been verified as stable are called, and switching uses
    /// the Ctrl+Win+Left/Right shell shortcut rather than the internal switch methods, whose
    /// vtable layout changes between Windows releases.
    /// </remarks>
    public sealed class VirtualDesktopService
    {
        private const string VirtualDesktopsKey =
            @"SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\VirtualDesktops";
        private const string RemoteSystemProvidersKey =
            @"SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\RemoteSystemProviders";

        private static readonly Guid ClsidImmersiveShell = new Guid("C2F03A33-21F5-47FA-B4BB-156362A2F239");
        private static readonly Guid ClsidVirtualDesktopManagerInternal =
            new Guid("C5E0CDCA-7B6E-41B2-9FC4-D93975CC467B");

        private static readonly string[] RemoteClientProcessNames =
        {
            "msrdc", "mstsc", "windowsapp", "windows365", "rdclient"
        };

        private readonly object gate = new object();
        private IVirtualDesktopManagerInternal desktopManagerInternal;
        private bool internalManagerUnavailable;
        private bool directSwitchUnavailable;
        private IVirtualDesktopManager publicDesktopManager;
        private bool publicManagerUnavailable;

        public VirtualDesktopSnapshot Read()
        {
            lock (gate)
            {
                var comOrder = TryReadComDesktops();
                var registryIds = ReadRegistryDesktopIds();

                var ids = comOrder.Count > 0 ? comOrder : registryIds;
                if (ids.Count == 0)
                {
                    return VirtualDesktopSnapshot.Empty;
                }

                var activeId = TryReadComCurrentDesktop() ?? ReadRegistryCurrentDesktop();
                var activeIndex = activeId.HasValue ? ids.IndexOf(activeId.Value) : -1;

                // The shell lists remote (Windows App / Cloud PC) desktops that Task view can
                // switch to, but they never get a registry entry, so that difference is the
                // authoritative remote signal. It only holds when the registry actually returned
                // its desktop list; an empty list means "unknown", not "everything is remote".
                var remoteIds = new List<Guid>();
                if (comOrder.Count > 0 && registryIds.Count > 0)
                {
                    foreach (var id in ids)
                    {
                        if (!registryIds.Contains(id))
                        {
                            remoteIds.Add(id);
                        }
                    }
                }

                // Naming a remote desktop is the only reason to walk the window list, so skip
                // that scan entirely while no remote desktop is present.
                var remoteWindows = remoteIds.Count > 0
                    ? ReadRemoteClientWindows()
                    : new Dictionary<Guid, string>();

                var desktops = new List<VirtualDesktopInfo>(ids.Count);
                for (var i = 0; i < ids.Count; i++)
                {
                    var id = ids[i];
                    var isRemote = remoteIds.Contains(id);
                    var name = ReadRegistryDesktopName(id);
                    if (string.IsNullOrWhiteSpace(name) && isRemote)
                    {
                        string remoteTitle;
                        name = remoteWindows.TryGetValue(id, out remoteTitle) && !string.IsNullOrWhiteSpace(remoteTitle)
                            ? remoteTitle
                            : "Remote";
                    }

                    if (string.IsNullOrWhiteSpace(name))
                    {
                        name = "Desktop " + (i + 1);
                    }

                    desktops.Add(new VirtualDesktopInfo(id, name, isRemote));
                }

                AppendDisconnectedRemoteSystems(desktops);

                return new VirtualDesktopSnapshot(desktops, activeIndex);
            }
        }

        /// <summary>
        /// Adds remote systems that are registered with Task view but have no live desktop, so a
        /// disconnected Cloud PC still shows up (and can be connected to) from the device.
        /// </summary>
        private void AppendDisconnectedRemoteSystems(List<VirtualDesktopInfo> desktops)
        {
            var registered = ReadRegisteredRemoteSystems();
            if (registered.Count == 0)
            {
                return;
            }

            foreach (var entry in registered)
            {
                var alreadyLive = false;
                foreach (var desktop in desktops)
                {
                    if (desktop.IsRemote && NamesMatch(desktop.Name, entry.Value))
                    {
                        alreadyLive = true;
                        break;
                    }
                }

                if (alreadyLive)
                {
                    continue;
                }

                // Hand the shell the registered remote system so the connect attaches the Task
                // view desktop, rather than launching the Windows App as a separate window.
                desktops.Add(new VirtualDesktopInfo(entry.Key.SystemId, entry.Value, true, true,
                    entry.Key.Provider + "\\" + entry.Key.SystemId));
            }
        }

        private static bool NamesMatch(string candidate, string registeredName)
        {
            if (string.IsNullOrWhiteSpace(candidate) || string.IsNullOrWhiteSpace(registeredName))
            {
                return false;
            }

            if (candidate.Equals(registeredName, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }

            // Session window titles and taskbar entries decorate the Cloud PC name with the pool
            // and account, e.g. "name (devcenter-x / Pool) (user@contoso.com)". Requiring a
            // separator after the prefix keeps similarly named machines apart, so "nmaliwa-x"
            // never matches "nmaliwa2-x".
            return StartsWithAtBoundary(candidate, registeredName) || StartsWithAtBoundary(registeredName, candidate);
        }

        private static bool StartsWithAtBoundary(string text, string prefix)
        {
            if (text.Length <= prefix.Length || !text.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }

            var next = text[prefix.Length];
            return next == ' ' || next == '(';
        }

        private static Dictionary<RemoteSystemKey, string> ReadRegisteredRemoteSystems()
        {
            var result = new Dictionary<RemoteSystemKey, string>();
            try
            {
                using (var providers = Registry.CurrentUser.OpenSubKey(RemoteSystemProvidersKey))
                {
                    if (providers == null)
                    {
                        return result;
                    }

                    foreach (var providerName in providers.GetSubKeyNames())
                    {
                        using (var provider = providers.OpenSubKey(providerName))
                        {
                            if (provider == null)
                            {
                                continue;
                            }

                            foreach (var systemId in provider.GetSubKeyNames())
                            {
                                using (var system = provider.OpenSubKey(systemId))
                                {
                                    var displayName = system?.GetValue("displayName") as string;
                                    if (string.IsNullOrWhiteSpace(displayName))
                                    {
                                        continue;
                                    }

                                    Guid id;
                                    if (!Guid.TryParse(systemId, out id))
                                    {
                                        id = Guid.Empty;
                                    }

                                    result[new RemoteSystemKey(providerName, systemId, id)] = displayName;
                                }
                            }
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                HostLog.Write("Registered remote system read failed.", ex);
            }

            return result;
        }

        private struct RemoteSystemKey
        {
            public RemoteSystemKey(string provider, string systemIdText, Guid systemId)
            {
                Provider = provider;
                SystemIdText = systemIdText;
                SystemId = systemId;
            }

            public string Provider { get; }
            public string SystemIdText { get; }
            public Guid SystemId { get; }
        }

        /// <summary>
        /// Switches to <paramref name="targetIndex"/>. Jumps straight to the target through the
        /// shell's virtual desktop manager, falling back to stepping with the
        /// <c>Ctrl+Win+Left/Right</c> shortcut if that call is unavailable on this Windows build.
        /// </summary>
        public bool SwitchTo(int targetIndex)
        {
            var snapshot = Read();
            if (targetIndex < 0 || targetIndex >= snapshot.Desktops.Count)
            {
                HostLog.Write("Desktop switch ignored; index out of range. index=" + targetIndex +
                    " count=" + snapshot.Desktops.Count);
                return false;
            }

            if (snapshot.ActiveIndex == targetIndex)
            {
                HostLog.Write("Desktop switch ignored; already active. index=" + targetIndex);
                return true;
            }

            var target = snapshot.Desktops[targetIndex];
            if (target.IsDisconnected)
            {
                // Connecting a remote machine is intentionally not automated: the shell calls that
                // would attach a Task view desktop create a brand new tile instead of reusing the
                // registered one, which corrupts the Windows 365 Switch state. Connect it from
                // Task view; the tile becomes switchable here as soon as the session is up.
                HostLog.Write("Desktop switch ignored; " + target.Name +
                    " has no live session. Connect it from Task view first.");
                return false;
            }

            if (TryDirectSwitch(targetIndex))
            {
                HostLog.Write("Desktop switch finished directly. index=" + targetIndex);
                return true;
            }

            if (snapshot.ActiveIndex < 0)
            {
                HostLog.Write("Desktop switch ignored; active desktop is unknown.");
                return false;
            }

            return SwitchByStepping(snapshot.ActiveIndex, targetIndex, snapshot.Desktops.Count);
        }

        /// <summary>
        /// Asks the shell to activate the target desktop in one hop. This also works for a remote
        /// (Windows App / Cloud PC) desktop.
        /// </summary>
        private bool TryDirectSwitch(int targetIndex)
        {
            if (directSwitchUnavailable)
            {
                return false;
            }

            var manager = GetInternalManager();
            if (manager == null)
            {
                return false;
            }

            try
            {
                IObjectArray array;
                manager.GetDesktops(out array);
                if (array == null)
                {
                    return false;
                }

                int count;
                array.GetCount(out count);
                if (targetIndex >= count)
                {
                    Marshal.ReleaseComObject(array);
                    return false;
                }

                var iid = typeof(IVirtualDesktop).GUID;
                object desktopObject;
                array.GetAt(targetIndex, ref iid, out desktopObject);
                Marshal.ReleaseComObject(array);

                var desktop = desktopObject as IVirtualDesktop;
                if (desktop == null)
                {
                    return false;
                }

                var targetId = desktop.GetId();
                // The shell expects the switch to come from a foreground shell window; without
                // this the call can be accepted but leave the desktop unchanged.
                var tray = FindWindow("Shell_TrayWnd", string.Empty);
                if (tray != IntPtr.Zero)
                {
                    SetForegroundWindow(tray);
                }

                manager.SwitchDesktop(desktop);
                Marshal.ReleaseComObject(desktop);

                for (var attempt = 0; attempt < 20; attempt++)
                {
                    Thread.Sleep(100);
                    var current = TryReadComCurrentDesktop();
                    if (current.HasValue && current.Value == targetId)
                    {
                        return true;
                    }
                }

                HostLog.Write("Direct desktop switch did not take effect; falling back to stepping.");
                return false;
            }
            catch (Exception ex)
            {
                HostLog.Write("Direct desktop switch unavailable; falling back to stepping.", ex);
                directSwitchUnavailable = true;
                return false;
            }
        }

        private bool SwitchByStepping(int activeIndex, int targetIndex, int desktopCount)
        {
            var index = activeIndex;
            var maxSteps = desktopCount * 2;
            for (var step = 0; step < maxSteps && index != targetIndex; step++)
            {
                var right = index < targetIndex;
                SendSwitchShortcut(right);
                var moved = false;
                for (var attempt = 0; attempt < 12; attempt++)
                {
                    Thread.Sleep(100);
                    var current = Read();
                    if (current.ActiveIndex >= 0 && current.ActiveIndex != index)
                    {
                        index = current.ActiveIndex;
                        moved = true;
                        break;
                    }
                }

                if (!moved)
                {
                    HostLog.Write("Desktop switch stalled. index=" + index + " target=" + targetIndex);
                    return false;
                }
            }

            HostLog.Write("Desktop switch finished by stepping. index=" + index + " target=" + targetIndex);
            return index == targetIndex;
        }

        private List<Guid> TryReadComDesktops()
        {
            var result = new List<Guid>();
            var manager = GetInternalManager();
            if (manager == null)
            {
                return result;
            }

            try
            {
                IObjectArray array;
                manager.GetDesktops(out array);
                if (array == null)
                {
                    return result;
                }

                int count;
                array.GetCount(out count);
                var iid = typeof(IVirtualDesktop).GUID;
                for (var i = 0; i < count; i++)
                {
                    object desktop;
                    array.GetAt(i, ref iid, out desktop);
                    var typed = desktop as IVirtualDesktop;
                    if (typed == null)
                    {
                        continue;
                    }

                    result.Add(typed.GetId());
                    Marshal.ReleaseComObject(typed);
                }

                Marshal.ReleaseComObject(array);
            }
            catch (Exception ex)
            {
                HostLog.Write("Virtual desktop enumeration failed; falling back to registry.", ex);
                desktopManagerInternal = null;
                internalManagerUnavailable = true;
                result.Clear();
            }

            return result;
        }

        private Guid? TryReadComCurrentDesktop()
        {
            var manager = GetInternalManager();
            if (manager == null)
            {
                return null;
            }

            try
            {
                var current = manager.GetCurrentDesktop();
                if (current == null)
                {
                    return null;
                }

                var id = current.GetId();
                Marshal.ReleaseComObject(current);
                return id;
            }
            catch (Exception ex)
            {
                HostLog.Write("Current virtual desktop lookup failed.", ex);
                desktopManagerInternal = null;
                internalManagerUnavailable = true;
                return null;
            }
        }

        private IVirtualDesktopManagerInternal GetInternalManager()
        {
            if (desktopManagerInternal != null)
            {
                return desktopManagerInternal;
            }

            if (internalManagerUnavailable)
            {
                return null;
            }

            try
            {
                var shellType = Type.GetTypeFromCLSID(ClsidImmersiveShell);
                var shell = (IServiceProvider10)Activator.CreateInstance(shellType);
                var service = ClsidVirtualDesktopManagerInternal;
                var iid = typeof(IVirtualDesktopManagerInternal).GUID;
                var manager = (IVirtualDesktopManagerInternal)shell.QueryService(ref service, ref iid);
                // Touch the interface once so an unsupported vtable layout is detected here.
                var count = manager.GetCount();
                HostLog.Write("Virtual desktop internal manager ready. count=" + count);
                desktopManagerInternal = manager;
                return desktopManagerInternal;
            }
            catch (Exception ex)
            {
                HostLog.Write("Virtual desktop internal manager unavailable; using registry only.", ex);
                internalManagerUnavailable = true;
                return null;
            }
        }

        private IVirtualDesktopManager GetPublicManager()
        {
            if (publicDesktopManager != null)
            {
                return publicDesktopManager;
            }

            if (publicManagerUnavailable)
            {
                return null;
            }

            try
            {
                publicDesktopManager = (IVirtualDesktopManager)new CVirtualDesktopManager();
                return publicDesktopManager;
            }
            catch (Exception ex)
            {
                HostLog.Write("Virtual desktop manager (public) unavailable.", ex);
                publicManagerUnavailable = true;
                return null;
            }
        }

        private Dictionary<Guid, string> ReadRemoteClientWindows()
        {
            var result = new Dictionary<Guid, string>();
            var manager = GetPublicManager();
            if (manager == null)
            {
                return result;
            }

            try
            {
                // One process snapshot per scan; a per-window Process.GetProcessById() would
                // re-query the whole system process table for every visible window.
                var processNames = new Dictionary<int, string>();
                foreach (var process in System.Diagnostics.Process.GetProcesses())
                {
                    try
                    {
                        processNames[process.Id] = process.ProcessName;
                    }
                    catch
                    {
                        // Process exited between enumeration and read.
                    }
                    finally
                    {
                        process.Dispose();
                    }
                }

                EnumWindows((hwnd, param) =>
                {
                    if (!IsWindowVisible(hwnd))
                    {
                        return true;
                    }

                    int processId;
                    GetWindowThreadProcessId(hwnd, out processId);
                    string processName;
                    if (!processNames.TryGetValue(processId, out processName))
                    {
                        return true;
                    }

                    var match = false;
                    foreach (var candidate in RemoteClientProcessNames)
                    {
                        if (processName.IndexOf(candidate, StringComparison.OrdinalIgnoreCase) >= 0)
                        {
                            match = true;
                            break;
                        }
                    }

                    if (!match)
                    {
                        return true;
                    }

                    Guid desktopId;
                    if (manager.GetWindowDesktopId(hwnd, out desktopId) != 0 || desktopId == Guid.Empty)
                    {
                        return true;
                    }

                    var title = new StringBuilder(256);
                    GetWindowText(hwnd, title, title.Capacity);
                    var text = title.ToString().Trim();
                    if (text.Length == 0)
                    {
                        return true;
                    }

                    if (!result.ContainsKey(desktopId))
                    {
                        result[desktopId] = text;
                    }

                    return true;
                }, IntPtr.Zero);
            }
            catch (Exception ex)
            {
                HostLog.Write("Remote desktop window scan failed.", ex);
            }

            return result;
        }

        private static List<Guid> ReadRegistryDesktopIds()
        {
            var result = new List<Guid>();
            try
            {
                using (var key = Registry.CurrentUser.OpenSubKey(VirtualDesktopsKey))
                {
                    var blob = key?.GetValue("VirtualDesktopIDs") as byte[];
                    if (blob == null)
                    {
                        return result;
                    }

                    for (var offset = 0; offset + 16 <= blob.Length; offset += 16)
                    {
                        var bytes = new byte[16];
                        Array.Copy(blob, offset, bytes, 0, 16);
                        result.Add(new Guid(bytes));
                    }
                }
            }
            catch (Exception ex)
            {
                HostLog.Write("Virtual desktop registry read failed.", ex);
            }

            return result;
        }

        private static Guid? ReadRegistryCurrentDesktop()
        {
            try
            {
                using (var key = Registry.CurrentUser.OpenSubKey(VirtualDesktopsKey))
                {
                    var blob = key?.GetValue("CurrentVirtualDesktop") as byte[];
                    if (blob == null || blob.Length < 16)
                    {
                        return null;
                    }

                    var bytes = new byte[16];
                    Array.Copy(blob, bytes, 16);
                    return new Guid(bytes);
                }
            }
            catch (Exception ex)
            {
                HostLog.Write("Current virtual desktop registry read failed.", ex);
                return null;
            }
        }

        private static string ReadRegistryDesktopName(Guid id)
        {
            try
            {
                using (var key = Registry.CurrentUser.OpenSubKey(
                    VirtualDesktopsKey + @"\Desktops\{" + id.ToString().ToUpperInvariant() + "}"))
                {
                    return key?.GetValue("Name") as string;
                }
            }
            catch
            {
                return null;
            }
        }

        private static void SendSwitchShortcut(bool right)
        {
            var arrow = right ? VK_RIGHT : VK_LEFT;
            var inputs = new[]
            {
                MakeKey(VK_CONTROL, false, false),
                MakeKey(VK_LWIN, false, true),
                MakeKey(arrow, false, true),
                MakeKey(arrow, true, true),
                MakeKey(VK_LWIN, true, true),
                MakeKey(VK_CONTROL, true, false)
            };

            var sent = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<INPUT>());
            if (sent != inputs.Length)
            {
                HostLog.Write("Desktop switch keystroke incomplete. sent=" + sent +
                    " error=" + Marshal.GetLastWin32Error());
            }
        }

        private static INPUT MakeKey(ushort virtualKey, bool up, bool extended)
        {
            uint flags = 0;
            if (up)
            {
                flags |= KEYEVENTF_KEYUP;
            }

            if (extended)
            {
                flags |= KEYEVENTF_EXTENDEDKEY;
            }

            return new INPUT
            {
                type = INPUT_KEYBOARD,
                U = new InputUnion
                {
                    ki = new KEYBDINPUT
                    {
                        wVk = virtualKey,
                        wScan = 0,
                        dwFlags = flags,
                        time = 0,
                        dwExtraInfo = IntPtr.Zero
                    }
                }
            };
        }

        private const uint INPUT_KEYBOARD = 1;
        private const uint KEYEVENTF_EXTENDEDKEY = 0x0001;
        private const uint KEYEVENTF_KEYUP = 0x0002;
        private const ushort VK_CONTROL = 0x11;
        private const ushort VK_LWIN = 0x5B;
        private const ushort VK_LEFT = 0x25;
        private const ushort VK_RIGHT = 0x27;

        private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr param);

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr param);

        [DllImport("user32.dll")]
        private static extern bool IsWindowVisible(IntPtr hwnd);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int count);

        [DllImport("user32.dll")]
        private static extern int GetWindowThreadProcessId(IntPtr hwnd, out int processId);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern uint SendInput(uint count, INPUT[] inputs, int size);

        [DllImport("user32.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern IntPtr FindWindow(string className, string windowName);

        [DllImport("user32.dll")]
        private static extern bool SetForegroundWindow(IntPtr hwnd);

        [StructLayout(LayoutKind.Sequential)]
        private struct INPUT
        {
            public uint type;
            public InputUnion U;
        }

        [StructLayout(LayoutKind.Explicit)]
        private struct InputUnion
        {
            [FieldOffset(0)] public MOUSEINPUT mi;
            [FieldOffset(0)] public KEYBDINPUT ki;
            [FieldOffset(0)] public HARDWAREINPUT hi;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct KEYBDINPUT
        {
            public ushort wVk;
            public ushort wScan;
            public uint dwFlags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct MOUSEINPUT
        {
            public int dx;
            public int dy;
            public uint mouseData;
            public uint dwFlags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct HARDWAREINPUT
        {
            public uint uMsg;
            public ushort wParamL;
            public ushort wParamH;
        }

        [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("6D5140C1-7436-11CE-8034-00AA006009FA")]
        private interface IServiceProvider10
        {
            [return: MarshalAs(UnmanagedType.IUnknown)]
            object QueryService(ref Guid service, ref Guid riid);
        }

        [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("92CA9DCD-5622-4BBA-A805-5E9F541BD8C9")]
        private interface IObjectArray
        {
            void GetCount(out int count);
            void GetAt(int index, ref Guid iid, [MarshalAs(UnmanagedType.Interface)] out object obj);
        }

        // Only the leading slots are declared: GetName()/IsRemote() move between Windows
        // releases, so they are never called.
        [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("3F07F4BE-B107-441A-AF0F-39D82529072C")]
        private interface IVirtualDesktop
        {
            bool IsViewVisible(object view);
            Guid GetId();
        }

        // Declared only as far as SwitchDesktop. The remote-desktop slots beyond this point are
        // deliberately left out: calling CreateRemoteDesktop attaches a brand new Task view tile
        // rather than reusing the registered remote machine, which corrupts Windows 365 Switch.
        [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("53F5CA0B-158F-4124-900C-057158060B27")]
        private interface IVirtualDesktopManagerInternal
        {
            int GetCount();
            void MoveViewToDesktop(object view, IVirtualDesktop desktop);
            bool CanViewMoveDesktops(object view);
            IVirtualDesktop GetCurrentDesktop();
            void GetDesktops(out IObjectArray desktops);
            [PreserveSig] int GetAdjacentDesktop(IVirtualDesktop from, int direction, out IVirtualDesktop desktop);
            void SwitchDesktop(IVirtualDesktop desktop);
        }

        [ComImport, Guid("AA509086-5CA9-4C25-8F95-589D3C07B48A")]
        private class CVirtualDesktopManager
        {
        }

        [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("A5CD92FF-29BE-454C-8D04-D82879FB3F1B")]
        private interface IVirtualDesktopManager
        {
            [PreserveSig] int IsWindowOnCurrentVirtualDesktop(IntPtr hwnd, out int onCurrentDesktop);
            [PreserveSig] int GetWindowDesktopId(IntPtr hwnd, out Guid desktopId);
            [PreserveSig] int MoveWindowToDesktop(IntPtr hwnd, ref Guid desktopId);
        }
    }

    public sealed class VirtualDesktopInfo
    {
        public VirtualDesktopInfo(Guid id, string name, bool isRemote, bool isDisconnected = false,
            string remotePath = null)
        {
            Id = id;
            Name = name ?? string.Empty;
            IsRemote = isRemote;
            IsDisconnected = isDisconnected;
            RemotePath = remotePath;
        }

        public Guid Id { get; }
        public string Name { get; }
        public bool IsRemote { get; }

        /// <summary>Registered with Task view but with no live session behind it.</summary>
        public bool IsDisconnected { get; }

        /// <summary>
        /// Shell identifier for a registered remote system, formatted as
        /// <c>PackageFamilyName\SystemId</c>. Used to attach the Task view desktop on connect.
        /// </summary>
        public string RemotePath { get; }
    }

    public sealed class VirtualDesktopSnapshot
    {
        public static readonly VirtualDesktopSnapshot Empty =
            new VirtualDesktopSnapshot(new List<VirtualDesktopInfo>(), -1);

        public VirtualDesktopSnapshot(IReadOnlyList<VirtualDesktopInfo> desktops, int activeIndex)
        {
            Desktops = desktops ?? new List<VirtualDesktopInfo>();
            ActiveIndex = activeIndex >= 0 && activeIndex < Desktops.Count ? activeIndex : -1;
        }

        public IReadOnlyList<VirtualDesktopInfo> Desktops { get; }
        public int ActiveIndex { get; }

        public CompanionDesktopState ToCompanionState(ushort acknowledgedSwitchSequence)
        {
            var desktops = new List<CompanionDesktopInfo>(Desktops.Count);
            foreach (var desktop in Desktops)
            {
                desktops.Add(new CompanionDesktopInfo(desktop.Name, desktop.IsRemote, desktop.IsDisconnected));
            }

            return new CompanionDesktopState(desktops, ActiveIndex, acknowledgedSwitchSequence);
        }

        public string Describe()
        {
            if (Desktops.Count == 0)
            {
                return "no desktops";
            }

            var parts = new List<string>(Desktops.Count);
            for (var i = 0; i < Desktops.Count; i++)
            {
                parts.Add((i == ActiveIndex ? "*" : string.Empty) + (i + 1) + ":" + Desktops[i].Name +
                    (Desktops[i].IsRemote ? "(remote" + (Desktops[i].IsDisconnected ? ",offline)" : ")") : string.Empty));
            }

            return string.Join(" ", parts);
        }
    }
}
