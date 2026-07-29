using System;
using System.Runtime.InteropServices;
using System.Windows.Automation;

namespace X3LaptopCompanion
{
    /// <summary>
    /// Reads and toggles the Windows 11 shell microphone privacy indicator (the taskbar
    /// "Microphone Muted: Microsoft Teams ..." item). That element lives in explorer.exe, so it is
    /// NOT DWM-cloaked when a full-screen Remote Desktop (msrdc) window is foreground -- unlike the
    /// Teams window's own UIA tree. It exposes the UIA InvokePattern, giving a cloak-proof and
    /// keystroke-free way to read true mute state and toggle it (Win+Alt+K would be forwarded to the
    /// remote machine; a UIA invoke is not).
    /// </summary>
    public sealed class MicIndicatorState
    {
        public bool Present { get; set; }
        public bool Muted { get; set; }
        public bool ReferencesTeams { get; set; }
        public string Text { get; set; } = string.Empty;

        public string FirstLine
        {
            get
            {
                if (string.IsNullOrEmpty(Text))
                {
                    return string.Empty;
                }

                var index = Text.IndexOfAny(new[] { '\r', '\n' });
                return index >= 0 ? Text.Substring(0, index) : Text;
            }
        }
    }

    public static class MicIndicator
    {
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

        private static readonly string[] TrayClasses = { "Shell_TrayWnd", "Shell_SecondaryTrayWnd" };

        public static MicIndicatorState Read()
        {
            var state = new MicIndicatorState();
            var element = FindIndicatorElement(out var name);
            if (element == null)
            {
                return state;
            }

            state.Present = true;
            state.Text = name;
            state.Muted = name.IndexOf("microphone muted", StringComparison.OrdinalIgnoreCase) >= 0;
            state.ReferencesTeams = name.IndexOf("teams", StringComparison.OrdinalIgnoreCase) >= 0;
            return state;
        }

        public static bool TryToggle()
        {
            var element = FindIndicatorElement(out var name);
            if (element == null)
            {
                HostLog.Write("Mic indicator toggle failed; indicator not present in the tray.");
                return false;
            }

            try
            {
                if (element.TryGetCurrentPattern(InvokePattern.Pattern, out var pattern))
                {
                    ((InvokePattern)pattern).Invoke();
                    HostLog.Write("Mic indicator invoked. text=\"" + FirstLine(name) + "\"");
                    return true;
                }

                HostLog.Write("Mic indicator toggle failed; InvokePattern is not supported by the element.");
                return false;
            }
            catch (Exception ex)
            {
                HostLog.Write("Mic indicator invoke error. " + ex.Message);
                return false;
            }
        }

        private static AutomationElement FindIndicatorElement(out string name)
        {
            name = string.Empty;
            foreach (var trayClass in TrayClasses)
            {
                var hwnd = FindWindow(trayClass, null);
                if (hwnd == IntPtr.Zero)
                {
                    continue;
                }

                AutomationElement root;
                try
                {
                    root = AutomationElement.FromHandle(hwnd);
                }
                catch (Exception ex)
                {
                    HostLog.Write("Mic indicator FromHandle failed for " + trayClass + ". " + ex.Message);
                    continue;
                }

                if (root == null)
                {
                    continue;
                }

                AutomationElementCollection descendants;
                try
                {
                    descendants = root.FindAll(TreeScope.Descendants, Condition.TrueCondition);
                }
                catch (Exception ex)
                {
                    HostLog.Write("Mic indicator FindAll failed for " + trayClass + ". " + ex.Message);
                    continue;
                }

                foreach (AutomationElement element in descendants)
                {
                    string elementName;
                    try
                    {
                        elementName = element.Current.Name ?? string.Empty;
                    }
                    catch
                    {
                        continue;
                    }

                    if (elementName.Length == 0)
                    {
                        continue;
                    }

                    if (IsMicIndicatorName(elementName))
                    {
                        name = elementName;
                        return element;
                    }
                }
            }

            return null;
        }

        private static bool IsMicIndicatorName(string value)
        {
            // The shell privacy indicator name reads like "Microphone Muted: Microsoft Teams\n..." when muted,
            // and a "... is using your microphone" variant while live.
            if (value.IndexOf("microphone", StringComparison.OrdinalIgnoreCase) < 0)
            {
                return false;
            }

            return value.IndexOf("muted", StringComparison.OrdinalIgnoreCase) >= 0
                || value.IndexOf("using your microphone", StringComparison.OrdinalIgnoreCase) >= 0
                || value.IndexOf("in use", StringComparison.OrdinalIgnoreCase) >= 0
                || value.IndexOf("apps using", StringComparison.OrdinalIgnoreCase) >= 0;
        }

        private static string FirstLine(string value)
        {
            if (string.IsNullOrEmpty(value))
            {
                return string.Empty;
            }

            var index = value.IndexOfAny(new[] { '\r', '\n' });
            return index >= 0 ? value.Substring(0, index) : value;
        }
    }
}
