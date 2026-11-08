using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Automation;

namespace X3LaptopCompanion
{
    public sealed partial class TeamsController
    {
        public async Task<string> JoinMeetingMutedAsync(CalendarMeeting meeting, CancellationToken token)
        {
            token.ThrowIfCancellationRequested();
            if (!MeetingCalendarService.IsTeamsJoinUrl(meeting.JoinUrl))
                throw new InvalidOperationException("The meeting does not contain a valid Teams join link.");
            var existing = TeamsJoinWindows();
            if (existing.Any(w => FindJoinButton(w) != null || FindLeaveButton(w) != null))
                throw new InvalidOperationException("Close the existing Teams call or pre-join screen first.");

            token.ThrowIfCancellationRequested();
            var uri = new Uri(meeting.JoinUrl);
            using var launch = Process.Start(new ProcessStartInfo("msteams://teams.microsoft.com" + uri.PathAndQuery)
            {
                UseShellExecute = true
            });
            var deadline = DateTimeOffset.UtcNow.AddSeconds(75);
            AutomationElement preJoin = null;
            while (DateTimeOffset.UtcNow < deadline)
            {
                token.ThrowIfCancellationRequested();
                var candidates = TeamsJoinWindows().Where(w => FindJoinButton(w) != null).ToArray();
                if (candidates.Length > 1)
                    throw new InvalidOperationException("Multiple Teams pre-join windows are open; join stopped.");
                if (candidates.Length == 1)
                {
                    preJoin = candidates[0];
                    break;
                }
                await Task.Delay(300, token).ConfigureAwait(false);
            }
            if (preJoin == null)
                throw new TimeoutException("Teams did not open the pre-join screen.");

            await SetPreJoinMediaOffAsync(preJoin, camera: true, token).ConfigureAwait(false);
            await SetPreJoinMediaOffAsync(preJoin, camera: false, token).ConfigureAwait(false);
            // Re-read both after device changes. A missing or ambiguous control is never treated as off.
            if (ReadPreJoinMedia(preJoin, true).State != CompanionTriState.Off ||
                ReadPreJoinMedia(preJoin, false).State != CompanionTriState.Off)
                throw new InvalidOperationException("Join stopped: camera-off and microphone-muted could not be verified.");
            token.ThrowIfCancellationRequested();
            var join = FindJoinButton(preJoin);
            if (join == null || !TryInvokeButton(join.Element, join.Name))
                throw new InvalidOperationException("The Teams Join now button could not be invoked.");

            while (DateTimeOffset.UtcNow < deadline)
            {
                token.ThrowIfCancellationRequested();
                if (TeamsJoinWindows().Any(w => FindLeaveButton(w) != null))
                    return "Joined muted, camera off";
                await Task.Delay(400, token).ConfigureAwait(false);
            }
            throw new TimeoutException("Join requested muted, camera off; check Teams for lobby or admission.");
        }

        private static IReadOnlyList<AutomationElement> TeamsJoinWindows()
        {
            var processes = FindTeamsProcesses();
            var ids = processes.Select(p => p.Id).ToHashSet();
            foreach (var process in processes) process.Dispose();
            var windows = AutomationElement.RootElement.FindAll(TreeScope.Children, Condition.TrueCondition);
            var result = new List<AutomationElement>();
            foreach (AutomationElement window in windows)
            {
                if (ids.Contains(window.Current.ProcessId)) result.Add(window);
            }
            return result;
        }

        private static ButtonInfo FindJoinButton(AutomationElement root) =>
            FindButtonByNames(root, new[] { "Join now", "Join meeting" });

        private static ButtonInfo FindLeaveButton(AutomationElement root) =>
            FindButtonByAutomationId(root, "hangup-button") ??
            FindButtonByNames(root, new[] { "Leave", "Leave meeting", "Hang up" });

        internal static CompanionTriState ParsePreJoinMediaState(string name, bool camera)
        {
            var media = camera ? @"(?:camera|video)" : @"(?:mic|microphone)";
            var text = (name ?? string.Empty).Trim();
            var mutedAction = camera ? string.Empty : @"|Unmute(?: (?:the )?" + media + @")?";
            var liveAction = camera ? string.Empty : @"|Mute(?: (?:the )?" + media + @")?";
            var suffix = @"(?:\s*\([^)]*\))?$";
            if (Regex.IsMatch(text, @"^(?:Turn (?:the )?" + media + @" on" + mutedAction +
                @"|" + media + @"(?: is)? (?:off|muted))" + suffix, RegexOptions.IgnoreCase | RegexOptions.CultureInvariant))
                return CompanionTriState.Off;
            if (Regex.IsMatch(text, @"^(?:Turn (?:the )?" + media + @" off" + liveAction +
                @"|" + media + @"(?: is)? (?:on|unmuted))" + suffix, RegexOptions.IgnoreCase | RegexOptions.CultureInvariant))
                return CompanionTriState.On;
            return CompanionTriState.Unknown;
        }

        internal static CompanionTriState ResolvePreJoinMediaState(CompanionTriState named, ToggleState? toggle)
        {
            if (!toggle.HasValue) return named;
            var state = toggle == ToggleState.Off ? CompanionTriState.Off :
                toggle == ToggleState.On ? CompanionTriState.On : CompanionTriState.Unknown;
            return named != CompanionTriState.Unknown && named != state ? CompanionTriState.Unknown : state;
        }

        private static (AutomationElement Element, CompanionTriState State) ReadPreJoinMedia(
            AutomationElement root, bool camera)
        {
            var controls = root.FindAll(TreeScope.Descendants, new OrCondition(
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button),
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.CheckBox)));
            var candidates = new List<(AutomationElement Element, CompanionTriState State)>();
            foreach (AutomationElement control in controls)
            {
                if (control.Current.IsOffscreen) continue;
                var name = control.Current.Name;
                var named = ParsePreJoinMediaState(name, camera);
                var generic = Regex.IsMatch(name ?? string.Empty, camera ? @"^(Camera|Video)$" : @"^(Mic|Microphone)$",
                    RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
                if (named == CompanionTriState.Unknown && !generic) continue;
                ToggleState? toggle = null;
                if (control.TryGetCurrentPattern(TogglePattern.Pattern, out var pattern))
                    toggle = ((TogglePattern)pattern).Current.ToggleState;
                candidates.Add((control, ResolvePreJoinMediaState(named, toggle)));
            }
            return candidates.Count == 1 ? candidates[0] : (null, CompanionTriState.Unknown);
        }

        private static async Task SetPreJoinMediaOffAsync(AutomationElement root, bool camera, CancellationToken token)
        {
            var media = ReadPreJoinMedia(root, camera);
            var label = camera ? "camera" : "microphone";
            if (media.State == CompanionTriState.Off) return;
            if (media.State != CompanionTriState.On || media.Element == null)
                throw new InvalidOperationException("Join stopped: " + label + " state is unknown or ambiguous.");
            token.ThrowIfCancellationRequested();
            if (media.Element.TryGetCurrentPattern(TogglePattern.Pattern, out var pattern))
                ((TogglePattern)pattern).Toggle();
            else if (!TryInvokeButton(media.Element, label))
                throw new InvalidOperationException("Join stopped: " + label + " control could not be changed.");
            await Task.Delay(350, token).ConfigureAwait(false);
            if (ReadPreJoinMedia(root, camera).State != CompanionTriState.Off)
                throw new InvalidOperationException("Join stopped: " + label + " off could not be verified.");
        }
    }
}
