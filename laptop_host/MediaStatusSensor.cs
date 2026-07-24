using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace X3LaptopCompanion
{
    public sealed class MediaStatusSensor
    {
        private const int HResultNotFound = unchecked((int)0x80070490);
        private const string WebcamConsentStoreKey =
            @"SOFTWARE\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\webcam";
        private static DateTime lastWasapiDumpUtc = DateTime.MinValue;
        private readonly object teamsAudioProcessIdsLock = new object();
        private IReadOnlyCollection<int> teamsAudioProcessIds = Array.Empty<int>();

        public IReadOnlyCollection<int> TeamsAudioProcessIds
        {
            get
            {
                lock (teamsAudioProcessIdsLock)
                {
                    return teamsAudioProcessIds.ToList();
                }
            }
        }

        public CompanionTriState GetMicrophoneState(IReadOnlyCollection<int> processIds)
        {
            try
            {
                var state = GetTeamsMicrophoneState(processIds);
                DumpWasapiCaptureState(processIds, state);
                return state;
            }
            catch (Exception ex)
            {
                SetTeamsAudioProcessIds(new HashSet<int>());
                HostLog.Write("WASAPI microphone detection failed: " + ex.Message);
                return CompanionTriState.Unknown;
            }
        }

        public CompanionTriState GetCameraState()
        {
            try
            {
                return IsWebcamInUse() ? CompanionTriState.On : CompanionTriState.Off;
            }
            catch (Exception ex)
            {
                HostLog.Write("Webcam registry detection failed: " + ex.Message);
                return CompanionTriState.Unknown;
            }
        }

        private static bool IsWebcamInUse()
        {
            using (var key = Registry.LocalMachine.OpenSubKey(WebcamConsentStoreKey))
            {
                if (CheckRegForWebcamInUse(key))
                {
                    return true;
                }
            }

            using (var key = Registry.CurrentUser.OpenSubKey(WebcamConsentStoreKey))
            {
                return CheckRegForWebcamInUse(key);
            }
        }

        private static bool CheckRegForWebcamInUse(RegistryKey key)
        {
            if (key == null)
            {
                return false;
            }

            foreach (var subKeyName in key.GetSubKeyNames())
            {
                if (subKeyName == "NonPackaged")
                {
                    using (var nonPackagedKey = key.OpenSubKey(subKeyName))
                    {
                        if (nonPackagedKey == null)
                        {
                            continue;
                        }

                        foreach (var nonPackagedSubKeyName in nonPackagedKey.GetSubKeyNames())
                        {
                            using (var subKey = nonPackagedKey.OpenSubKey(nonPackagedSubKeyName))
                            {
                                if (HasActiveLastUsedTimeStop(subKey))
                                {
                                    return true;
                                }
                            }
                        }
                    }
                }
                else
                {
                    using (var subKey = key.OpenSubKey(subKeyName))
                    {
                        if (HasActiveLastUsedTimeStop(subKey))
                        {
                            return true;
                        }
                    }
                }
            }

            return false;
        }

        private static bool HasActiveLastUsedTimeStop(RegistryKey key)
        {
            if (key == null || !key.GetValueNames().Contains("LastUsedTimeStop"))
            {
                return false;
            }

            return key.GetValue("LastUsedTimeStop") is long endTime && endTime <= 0;
        }

        private static void DumpWasapiCaptureState(IReadOnlyCollection<int> processIds, CompanionTriState result)
        {
            if ((DateTime.UtcNow - lastWasapiDumpUtc) < TimeSpan.FromSeconds(8))
            {
                return;
            }

            lastWasapiDumpUtc = DateTime.UtcNow;
            var processSet = new HashSet<int>(processIds ?? Array.Empty<int>());
            HostLog.Write("WASAPI dump begin. result=" + result + " teamsPids=" +
                (processSet.Count == 0 ? "(none)" : string.Join(",", processSet)));

            object enumeratorObject = null;

            try
            {
                enumeratorObject = new MMDeviceEnumerator();
                var enumerator = (IMMDeviceEnumerator)enumeratorObject;
                var roles = new[] { ERole.Communications, ERole.Multimedia, ERole.Console };

                foreach (var role in roles)
                {
                    IMMDevice device = null;

                    try
                    {
                        var hr = enumerator.GetDefaultAudioEndpoint(EDataFlow.Capture, role, out device);
                        if (hr == HResultNotFound)
                        {
                            HostLog.Write("WASAPI endpoint role=" + role + " not found.");
                            continue;
                        }

                        if (hr != 0)
                        {
                            HostLog.Write("WASAPI endpoint role=" + role + " hr=0x" + hr.ToString("X8") + ".");
                            continue;
                        }

                        var deviceId = TryGetDeviceId(device);
                        var endpointMuteText = TryGetEndpointMuted(device, out var endpointMuted)
                            ? endpointMuted.ToString()
                            : "unavailable";
                        HostLog.Write("WASAPI endpoint role=" + role + " id=" + deviceId +
                            " endpointMuted=" + endpointMuteText + ".");
                        DumpDeviceSessions(device, processSet, role.ToString());
                    }
                    catch (Exception ex)
                    {
                        HostLog.Write("WASAPI endpoint role=" + role + " dump failed: " + ex.Message);
                    }
                    finally
                    {
                        ReleaseComObject(device);
                    }
                }
            }
            catch (Exception ex)
            {
                HostLog.Write("WASAPI dump failed: " + ex.Message);
            }
            finally
            {
                ReleaseComObject(enumeratorObject);
                HostLog.Write("WASAPI dump end.");
            }
        }

        private CompanionTriState GetTeamsMicrophoneState(IReadOnlyCollection<int> processIds)
        {
            var matchedAudioProcessIds = new HashSet<int>();
            var state = GetTeamsMicrophoneState(processIds, matchedAudioProcessIds);
            SetTeamsAudioProcessIds(matchedAudioProcessIds);
            return state;
        }

        private void SetTeamsAudioProcessIds(HashSet<int> processIds)
        {
            lock (teamsAudioProcessIdsLock)
            {
                teamsAudioProcessIds = processIds.ToList();
            }
        }

        private static CompanionTriState GetTeamsMicrophoneState(IReadOnlyCollection<int> processIds,
            HashSet<int> matchedAudioProcessIds)
        {
            var processSet = new HashSet<int>(processIds ?? Array.Empty<int>());
            object enumeratorObject = null;
            CompanionTriState? sessionState = null;

            try
            {
                enumeratorObject = new MMDeviceEnumerator();
                var enumerator = (IMMDeviceEnumerator)enumeratorObject;
                var roles = new[] { ERole.Communications, ERole.Multimedia, ERole.Console };

                foreach (var role in roles)
                {
                    IMMDevice device = null;

                    try
                    {
                        var hr = enumerator.GetDefaultAudioEndpoint(EDataFlow.Capture, role, out device);
                        if (hr == HResultNotFound)
                        {
                            continue;
                        }

                        Marshal.ThrowExceptionForHR(hr);
                        if (device == null)
                        {
                            continue;
                        }

                        if (TryGetEndpointMuted(device, out var endpointMuted) && endpointMuted)
                        {
                            return CompanionTriState.Off;
                        }

                        var deviceSessionState = GetDeviceTeamsSessionState(device, processSet, matchedAudioProcessIds);
                        if (deviceSessionState == CompanionTriState.Off)
                        {
                            return CompanionTriState.Off;
                        }

                        if (deviceSessionState.HasValue)
                        {
                            sessionState = deviceSessionState.Value;
                        }
                    }
                    finally
                    {
                        ReleaseComObject(device);
                    }
                }

                return sessionState ?? CompanionTriState.Unknown;
            }
            finally
            {
                ReleaseComObject(enumeratorObject);
            }
        }

        private static CompanionTriState? GetDeviceTeamsSessionState(IMMDevice device, HashSet<int> processSet,
            HashSet<int> matchedAudioProcessIds)
        {
            object sessionManagerObject = null;

            try
            {
                var sessionManagerId = typeof(IAudioSessionManager2).GUID;
                Marshal.ThrowExceptionForHR(device.Activate(ref sessionManagerId, ClsCtxAll, IntPtr.Zero,
                    out sessionManagerObject));
                return GetTeamsSessionState((IAudioSessionManager2)sessionManagerObject, processSet,
                    matchedAudioProcessIds);
            }
            finally
            {
                ReleaseComObject(sessionManagerObject);
            }
        }

        private static void DumpDeviceSessions(IMMDevice device, HashSet<int> processSet, string roleName)
        {
            object sessionManagerObject = null;
            IAudioSessionEnumerator sessionEnumerator = null;

            try
            {
                var sessionManagerId = typeof(IAudioSessionManager2).GUID;
                Marshal.ThrowExceptionForHR(device.Activate(ref sessionManagerId, ClsCtxAll, IntPtr.Zero,
                    out sessionManagerObject));
                var sessionManager = (IAudioSessionManager2)sessionManagerObject;
                Marshal.ThrowExceptionForHR(sessionManager.GetSessionEnumerator(out sessionEnumerator));
                Marshal.ThrowExceptionForHR(sessionEnumerator.GetCount(out var sessionCount));
                HostLog.Write("WASAPI sessions role=" + roleName + " count=" + sessionCount + ".");

                for (var i = 0; i < sessionCount; i++)
                {
                    IAudioSessionControl sessionControl = null;

                    try
                    {
                        Marshal.ThrowExceptionForHR(sessionEnumerator.GetSession(i, out sessionControl));
                        var stateText = TryGetSessionState(sessionControl, out var state)
                            ? state.ToString()
                            : "unavailable";
                        var processIdText = TryGetSessionProcessId(sessionControl, out var processId)
                            ? processId.ToString()
                            : "unavailable";
                        var displayName = GetSessionDisplayName(sessionControl);
                        var instanceId = GetSessionInstanceIdentifier(sessionControl);
                        var sessionMutedText = TryGetSessionMuted(sessionControl, out var sessionMuted)
                            ? sessionMuted.ToString()
                            : "unavailable";
                        var sessionVolumeText = TryGetSessionVolume(sessionControl, out var sessionVolume)
                            ? sessionVolume.ToString("0.000")
                            : "unavailable";
                        var matchesTeams = SessionMatchesTeams(sessionControl, processSet);

                        HostLog.Write("WASAPI session role=" + roleName + " index=" + i +
                            " state=" + stateText +
                            " pid=" + processIdText +
                            " matchesTeams=" + matchesTeams +
                            " muted=" + sessionMutedText +
                            " volume=" + sessionVolumeText +
                            " displayName=\"" + displayName + "\"" +
                            " instanceId=\"" + instanceId + "\".");
                    }
                    catch (Exception ex)
                    {
                        HostLog.Write("WASAPI session role=" + roleName + " index=" + i +
                            " dump failed: " + ex.Message);
                    }
                    finally
                    {
                        ReleaseComObject(sessionControl);
                    }
                }
            }
            catch (Exception ex)
            {
                HostLog.Write("WASAPI sessions role=" + roleName + " dump failed: " + ex.Message);
            }
            finally
            {
                ReleaseComObject(sessionEnumerator);
                ReleaseComObject(sessionManagerObject);
            }
        }

        private static CompanionTriState? GetTeamsSessionState(IAudioSessionManager2 sessionManager,
            HashSet<int> processIds, HashSet<int> matchedAudioProcessIds)
        {
            IAudioSessionEnumerator sessionEnumerator = null;
            var sawTeamsSession = false;
            var sawActiveTeamsSession = false;

            try
            {
                Marshal.ThrowExceptionForHR(sessionManager.GetSessionEnumerator(out sessionEnumerator));
                Marshal.ThrowExceptionForHR(sessionEnumerator.GetCount(out var sessionCount));

                for (var i = 0; i < sessionCount; i++)
                {
                    IAudioSessionControl sessionControl = null;

                    try
                    {
                        Marshal.ThrowExceptionForHR(sessionEnumerator.GetSession(i, out sessionControl));
                        if (!SessionMatchesTeams(sessionControl, processIds))
                        {
                            continue;
                        }

                        sawTeamsSession = true;
                        if (TryGetSessionProcessId(sessionControl, out var matchedProcessId) && matchedProcessId > 0)
                        {
                            matchedAudioProcessIds.Add((int)matchedProcessId);
                        }

                        if (TryGetSessionMuted(sessionControl, out var sessionMuted) && sessionMuted)
                        {
                            return CompanionTriState.Off;
                        }

                        Marshal.ThrowExceptionForHR(sessionControl.GetState(out var state));
                        if (state == AudioSessionState.Active)
                        {
                            sawActiveTeamsSession = true;
                        }
                    }
                    finally
                    {
                        ReleaseComObject(sessionControl);
                    }
                }

                if (sawActiveTeamsSession)
                {
                    return CompanionTriState.On;
                }

                return sawTeamsSession ? CompanionTriState.Unknown : null;
            }
            finally
            {
                ReleaseComObject(sessionEnumerator);
            }
        }

        private static bool SessionMatchesTeams(IAudioSessionControl sessionControl, HashSet<int> processIds)
        {
            if (TryGetSessionProcessId(sessionControl, out var processId) && processIds.Contains((int)processId))
            {
                return true;
            }

            return SessionDisplayNameLooksLikeTeams(sessionControl);
        }

        private static bool TryGetSessionState(IAudioSessionControl sessionControl, out AudioSessionState state)
        {
            state = AudioSessionState.Inactive;

            try
            {
                Marshal.ThrowExceptionForHR(sessionControl.GetState(out state));
                return true;
            }
            catch
            {
                state = AudioSessionState.Inactive;
                return false;
            }
        }

        private static bool TryGetSessionProcessId(IAudioSessionControl sessionControl, out uint processId)
        {
            processId = 0;
            var sessionControl2 = sessionControl as IAudioSessionControl2;
            if (sessionControl2 == null)
            {
                return false;
            }

            try
            {
                Marshal.ThrowExceptionForHR(sessionControl2.GetProcessId(out processId));
                return true;
            }
            catch
            {
                processId = 0;
                return false;
            }
        }

        private static bool TryGetSessionMuted(IAudioSessionControl sessionControl, out bool muted)
        {
            muted = false;
            var simpleAudioVolume = sessionControl as ISimpleAudioVolume;
            if (simpleAudioVolume == null)
            {
                return false;
            }

            try
            {
                Marshal.ThrowExceptionForHR(simpleAudioVolume.GetMute(out muted));
                return true;
            }
            catch
            {
                muted = false;
                return false;
            }
        }

        private static bool TryGetSessionVolume(IAudioSessionControl sessionControl, out float volume)
        {
            volume = 0;
            var simpleAudioVolume = sessionControl as ISimpleAudioVolume;
            if (simpleAudioVolume == null)
            {
                return false;
            }

            try
            {
                Marshal.ThrowExceptionForHR(simpleAudioVolume.GetMasterVolume(out volume));
                return true;
            }
            catch
            {
                volume = 0;
                return false;
            }
        }

        private static bool TryGetEndpointMuted(IMMDevice device, out bool muted)
        {
            object endpointVolumeObject = null;
            muted = false;

            try
            {
                var endpointVolumeId = typeof(IAudioEndpointVolume).GUID;
                Marshal.ThrowExceptionForHR(device.Activate(ref endpointVolumeId, ClsCtxAll, IntPtr.Zero,
                    out endpointVolumeObject));
                var endpointVolume = (IAudioEndpointVolume)endpointVolumeObject;
                Marshal.ThrowExceptionForHR(endpointVolume.GetMute(out muted));
                return true;
            }
            catch
            {
                muted = false;
                return false;
            }
            finally
            {
                ReleaseComObject(endpointVolumeObject);
            }
        }

        private static void ReleaseComObject(object value)
        {
            if (value != null && Marshal.IsComObject(value))
            {
                Marshal.ReleaseComObject(value);
            }
        }

        private static string TryGetDeviceId(IMMDevice device)
        {
            if (device == null)
            {
                return "(null)";
            }

            try
            {
                Marshal.ThrowExceptionForHR(device.GetId(out var id));
                return id ?? string.Empty;
            }
            catch (Exception ex)
            {
                return "unavailable:" + ex.Message;
            }
        }

        private static bool SessionDisplayNameLooksLikeTeams(IAudioSessionControl sessionControl)
        {
            var displayName = GetSessionDisplayName(sessionControl);
            return displayName.IndexOf("Teams", StringComparison.OrdinalIgnoreCase) >= 0 ||
                   displayName.IndexOf("ms-teams", StringComparison.OrdinalIgnoreCase) >= 0;
        }

        private static string GetSessionDisplayName(IAudioSessionControl sessionControl)
        {
            IntPtr displayNamePtr = IntPtr.Zero;

            try
            {
                Marshal.ThrowExceptionForHR(sessionControl.GetDisplayName(out displayNamePtr));
                return Marshal.PtrToStringUni(displayNamePtr) ?? string.Empty;
            }
            catch (Exception ex)
            {
                return "unavailable:" + ex.Message;
            }
            finally
            {
                if (displayNamePtr != IntPtr.Zero)
                {
                    Marshal.FreeCoTaskMem(displayNamePtr);
                }
            }
        }

        private static string GetSessionInstanceIdentifier(IAudioSessionControl sessionControl)
        {
            var sessionControl2 = sessionControl as IAudioSessionControl2;
            if (sessionControl2 == null)
            {
                return "unavailable";
            }

            IntPtr instanceIdPtr = IntPtr.Zero;

            try
            {
                Marshal.ThrowExceptionForHR(sessionControl2.GetSessionInstanceIdentifier(out instanceIdPtr));
                return Marshal.PtrToStringUni(instanceIdPtr) ?? string.Empty;
            }
            catch (Exception ex)
            {
                return "unavailable:" + ex.Message;
            }
            finally
            {
                if (instanceIdPtr != IntPtr.Zero)
                {
                    Marshal.FreeCoTaskMem(instanceIdPtr);
                }
            }
        }

        private const uint ClsCtxAll = 23;

        private enum EDataFlow
        {
            Render,
            Capture,
            All
        }

        private enum ERole
        {
            Console,
            Multimedia,
            Communications
        }

        [Flags]
        private enum DeviceState : uint
        {
            Active = 0x00000001
        }

        private enum AudioSessionState
        {
            Inactive = 0,
            Active = 1,
            Expired = 2
        }

        [ComImport]
        [Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
        private class MMDeviceEnumerator
        {
        }

        [ComImport]
        [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IMMDeviceEnumerator
        {
            int EnumAudioEndpoints(EDataFlow dataFlow, DeviceState dwStateMask, out IntPtr ppDevices);
            int GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role,
                [MarshalAs(UnmanagedType.Interface)] out IMMDevice ppEndpoint);
            int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string pwstrId,
                [MarshalAs(UnmanagedType.Interface)] out IMMDevice ppDevice);
            int RegisterEndpointNotificationCallback(IntPtr pClient);
            int UnregisterEndpointNotificationCallback(IntPtr pClient);
        }

        [ComImport]
        [Guid("0BD7A1BE-7A1A-44DB-8397-C0F5B184F278")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IMMDeviceCollection
        {
            int GetCount(out uint pcDevices);
            int Item(uint nDevice, [MarshalAs(UnmanagedType.Interface)] out IMMDevice ppDevice);
        }

        [ComImport]
        [Guid("D666063F-1587-4E43-81F1-B948E807363F")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IMMDevice
        {
            int Activate(ref Guid iid, uint dwClsCtx, IntPtr pActivationParams,
                [MarshalAs(UnmanagedType.IUnknown)] out object ppInterface);
            int OpenPropertyStore(uint stgmAccess, IntPtr ppProperties);
            int GetId([MarshalAs(UnmanagedType.LPWStr)] out string ppstrId);
            int GetState(out DeviceState pdwState);
        }

        [ComImport]
        [Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IAudioSessionManager2
        {
            int GetAudioSessionControl(IntPtr audioSessionGuid, uint streamFlags, out IAudioSessionControl sessionControl);
            int GetSimpleAudioVolume(IntPtr audioSessionGuid, uint streamFlags, out IntPtr audioVolume);
            int GetSessionEnumerator(out IAudioSessionEnumerator sessionEnum);
            int RegisterSessionNotification(IntPtr sessionNotification);
            int UnregisterSessionNotification(IntPtr sessionNotification);
            int RegisterDuckNotification([MarshalAs(UnmanagedType.LPWStr)] string sessionId, IntPtr duckNotification);
            int UnregisterDuckNotification(IntPtr duckNotification);
        }

        [ComImport]
        [Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IAudioSessionEnumerator
        {
            int GetCount(out int sessionCount);
            int GetSession(int sessionCount, out IAudioSessionControl session);
        }

        [ComImport]
        [Guid("F4B1A599-7266-4319-A8CA-E70ACB11E8CD")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IAudioSessionControl
        {
            int GetState(out AudioSessionState state);
            int GetDisplayName(out IntPtr displayName);
            int SetDisplayName([MarshalAs(UnmanagedType.LPWStr)] string value, ref Guid eventContext);
            int GetIconPath(out IntPtr iconPath);
            int SetIconPath([MarshalAs(UnmanagedType.LPWStr)] string value, ref Guid eventContext);
            int GetGroupingParam(out Guid groupingId);
            int SetGroupingParam(ref Guid groupingId, ref Guid eventContext);
            int RegisterAudioSessionNotification(IntPtr client);
            int UnregisterAudioSessionNotification(IntPtr client);
        }

        [ComImport]
        [Guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IAudioSessionControl2
        {
            int GetState(out AudioSessionState state);
            int GetDisplayName(out IntPtr displayName);
            int SetDisplayName([MarshalAs(UnmanagedType.LPWStr)] string value, ref Guid eventContext);
            int GetIconPath(out IntPtr iconPath);
            int SetIconPath([MarshalAs(UnmanagedType.LPWStr)] string value, ref Guid eventContext);
            int GetGroupingParam(out Guid groupingId);
            int SetGroupingParam(ref Guid groupingId, ref Guid eventContext);
            int RegisterAudioSessionNotification(IntPtr client);
            int UnregisterAudioSessionNotification(IntPtr client);
            int GetSessionIdentifier(out IntPtr retVal);
            int GetSessionInstanceIdentifier(out IntPtr retVal);
            int GetProcessId(out uint retVal);
            int IsSystemSoundsSession();
            int SetDuckingPreference(bool optOut);
        }

        [ComImport]
        [Guid("87CE5498-68D6-44E5-9215-6DA47EF883D8")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface ISimpleAudioVolume
        {
            int SetMasterVolume(float level, ref Guid eventContext);
            int GetMasterVolume(out float level);
            int SetMute([MarshalAs(UnmanagedType.Bool)] bool muted, ref Guid eventContext);
            int GetMute([MarshalAs(UnmanagedType.Bool)] out bool muted);
        }

        [ComImport]
        [Guid("5CDF2C82-841E-4546-9722-0CF74078229A")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IAudioEndpointVolume
        {
            int RegisterControlChangeNotify(IntPtr client);
            int UnregisterControlChangeNotify(IntPtr client);
            int GetChannelCount(out uint channelCount);
            int SetMasterVolumeLevel(float levelDb, ref Guid eventContext);
            int SetMasterVolumeLevelScalar(float level, ref Guid eventContext);
            int GetMasterVolumeLevel(out float levelDb);
            int GetMasterVolumeLevelScalar(out float level);
            int SetChannelVolumeLevel(uint channelNumber, float levelDb, ref Guid eventContext);
            int SetChannelVolumeLevelScalar(uint channelNumber, float level, ref Guid eventContext);
            int GetChannelVolumeLevel(uint channelNumber, out float levelDb);
            int GetChannelVolumeLevelScalar(uint channelNumber, out float level);
            int SetMute([MarshalAs(UnmanagedType.Bool)] bool muted, ref Guid eventContext);
            int GetMute([MarshalAs(UnmanagedType.Bool)] out bool muted);
            int GetVolumeStepInfo(out uint step, out uint stepCount);
            int VolumeStepUp(ref Guid eventContext);
            int VolumeStepDown(ref Guid eventContext);
            int QueryHardwareSupport(out uint hardwareSupportMask);
            int GetVolumeRange(out float minDb, out float maxDb, out float incrementDb);
        }
    }
}
