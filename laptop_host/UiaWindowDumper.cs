using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Automation;

namespace X3LaptopCompanion
{
    public sealed class UiaWindowDumper
    {
        private const int MaxDepth = 9;
        private const int MaxNodes = 1200;
        private const int MaxRawDepth = 24;
        private const int MaxRawNodes = 5000;
        private const int MaxEmbeddedBrowserDepth = 32;
        private const int MaxEmbeddedBrowserNodes = 8000;
        private const int MaxEmbeddedBrowserSearchDepth = 28;
        private const int MaxEmbeddedBrowserRoots = 20;
        private const int MaxProcessRoots = 120;
        private const int MaxProcessTreeDepth = 12;
        private const int MaxProcessTreeNodes = 2000;
        private const int MaxUnmuteMatches = 80;
        private const int MaxInterestingMatches = 120;
        private static readonly string[] InterestingAutomationIds =
        {
            "microphone-button",
            "camera-button",
            "raise-hand-button",
            "hangup-button",
            "callingButtons-showMoreBtn",
            "more-actions-button"
        };

        private static readonly string[] InterestingNameFragments =
        {
            "Mute",
            "Unmute",
            "mic",
            "microphone",
            "camera",
            "video",
            "Raise hand",
            "Lower hand",
            "More",
            "actions",
            "Leave",
            "Hang up"
        };

        public bool DumpWindow(string target)
        {
            if (!TryResolveWindow(target, out var hwnd, out var resolvedBy))
            {
                HostLog.Write("UIA dump target not found. target=\"" + (target ?? string.Empty) + "\"");
                return false;
            }

            HostLog.Write("UIA dump begin. target=\"" + target + "\" resolvedBy=\"" + resolvedBy +
                "\" hwnd=0x" + hwnd.ToInt64().ToString("X") + " title=\"" + GetWindowTitle(hwnd) + "\"");

            try
            {
                var root = AutomationElement.FromHandle(hwnd);
                if (root == null)
                {
                    HostLog.Write("UIA dump failed: AutomationElement.FromHandle returned null.");
                    return false;
                }

                var targetProcessId = TryGetRequestedProcessId(target, resolvedBy);
                if (!targetProcessId.HasValue)
                {
                    GetWindowThreadProcessId(hwnd, out var windowProcessId);
                    targetProcessId = windowProcessId;
                }

                HostLog.Write("UIA focused dump. targetPid=" + targetProcessId.Value +
                    " targetProcess=\"" + GetProcessName(targetProcessId.Value) + "\"");
                DumpProcessScopedTrees(root, targetProcessId.Value);
                DumpUnmuteButtonPaths(root);

                HostLog.Write("UIA dump end.");
                return true;
            }
            catch (Exception ex)
            {
                HostLog.Write("UIA dump failed: " + ex);
                return false;
            }
        }

        private static void DumpProcessScopedTrees(AutomationElement root, int processId)
        {
            var processRoots = new List<AutomationElement>();
            var visitedRuntimeIds = new HashSet<string>();
            FindProcessRoots(root, processId, TreeWalker.RawViewWalker, 0, processRoots, visitedRuntimeIds);

            HostLog.Write("UIA process-scoped roots begin. pid=" + processId + " count=" + processRoots.Count + ".");
            for (var i = 0; i < processRoots.Count; i++)
            {
                var processRoot = processRoots[i];
                HostLog.Write("UIA process-scoped root index=" + i + " " + DescribeElement(processRoot));
                HostLog.Write("UIA process-scoped root path index=" + i + " " + BuildPath(root, processRoot));

                var count = 0;
                HostLog.Write("UIA process-scoped tree begin. index=" + i + ".");
                DumpElement(processRoot, TreeWalker.RawViewWalker, "process-raw-" + i, 0,
                    MaxProcessTreeDepth, MaxProcessTreeNodes, ref count);
                HostLog.Write("UIA process-scoped tree end. index=" + i + " nodes=" + count + ".");
            }

            HostLog.Write("UIA process-scoped roots end.");
        }

        private static void DumpUnmuteButtonPaths(AutomationElement root)
        {
            var matches = new List<AutomationElement>();
            var visitedRuntimeIds = new HashSet<string>();
            FindUnmuteButtons(root, TreeWalker.RawViewWalker, 0, matches, visitedRuntimeIds);

            HostLog.Write("UIA unmute button paths begin. count=" + matches.Count + ".");
            for (var i = 0; i < matches.Count; i++)
            {
                HostLog.Write("UIA unmute button match index=" + i + " " + DescribeElement(matches[i]));
                HostLog.Write("UIA unmute button path index=" + i + " " + BuildPath(root, matches[i]));
            }

            HostLog.Write("UIA unmute button paths end.");
        }

        public void ListWindows(string filter)
        {
            var windows = GetTopLevelWindows()
                .Where(w => string.IsNullOrWhiteSpace(filter) || WindowMatchesFilter(w, filter))
                .ToList();

            HostLog.Write("UIA window list begin. filter=\"" + (filter ?? string.Empty) + "\" count=" + windows.Count);
            foreach (var window in windows)
            {
                HostLog.Write("UIA window hwnd=0x" + window.Hwnd.ToInt64().ToString("X") +
                    " pid=" + window.ProcessId +
                    " process=\"" + window.ProcessName + "\"" +
                    " title=\"" + window.Title + "\"");
            }

            HostLog.Write("UIA window list end.");
        }

        private static void DumpElement(AutomationElement element, TreeWalker walker, string viewName, int depth,
            int maxDepth, int maxNodes, ref int count)
        {
            if (element == null || count >= maxNodes)
            {
                return;
            }

            HostLog.Write("UIA " + viewName + " " + new string(' ', depth * 2) + DescribeElement(element));
            count++;

            if (depth >= maxDepth || count >= maxNodes)
            {
                return;
            }

            AutomationElement child = null;
            try
            {
                child = walker.GetFirstChild(element);
            }
            catch (Exception ex)
            {
                HostLog.Write("UIA " + viewName + " " + new string(' ', (depth + 1) * 2) +
                    "children unavailable: " + ex.Message);
                return;
            }

            while (child != null && count < maxNodes)
            {
                DumpElement(child, walker, viewName, depth + 1, maxDepth, maxNodes, ref count);
                try
                {
                    child = walker.GetNextSibling(child);
                }
                catch (Exception ex)
                {
                    HostLog.Write("UIA " + viewName + " " + new string(' ', (depth + 1) * 2) +
                        "sibling unavailable: " + ex.Message);
                    return;
                }
            }
        }

        private static void DumpInterestingPaths(AutomationElement root)
        {
            HostLog.Write("UIA interesting paths begin.");
            var matches = new List<AutomationElement>();
            var visitedRuntimeIds = new HashSet<string>();
            FindInterestingDescendants(root, TreeWalker.RawViewWalker, 0, ref matches, visitedRuntimeIds);

            HostLog.Write("UIA interesting paths count=" + matches.Count + ".");
            for (var i = 0; i < matches.Count; i++)
            {
                HostLog.Write("UIA interesting match index=" + i + " " + DescribeElement(matches[i]));
                HostLog.Write("UIA interesting path index=" + i + " " + BuildPath(root, matches[i]));
            }

            HostLog.Write("UIA interesting paths end.");
        }

        private static void DumpEmbeddedBrowserRoots(AutomationElement root)
        {
            var roots = new List<AutomationElement>();
            var visitedRuntimeIds = new HashSet<string>();
            FindEmbeddedBrowserRoots(root, TreeWalker.RawViewWalker, 0, roots, visitedRuntimeIds);

            HostLog.Write("UIA embedded-browser roots begin. count=" + roots.Count + ".");
            for (var i = 0; i < roots.Count; i++)
            {
                var browserRoot = roots[i];
                HostLog.Write("UIA embedded-browser root index=" + i + " " + DescribeElement(browserRoot));
                HostLog.Write("UIA embedded-browser root path index=" + i + " " + BuildPath(root, browserRoot));

                DumpInterestingPaths(browserRoot, "embedded-browser " + i);

                var browserCount = 0;
                HostLog.Write("UIA embedded-browser raw-view tree begin. index=" + i + ".");
                DumpElement(browserRoot, TreeWalker.RawViewWalker, "embedded-raw-" + i, 0,
                    MaxEmbeddedBrowserDepth, MaxEmbeddedBrowserNodes, ref browserCount);
                HostLog.Write("UIA embedded-browser raw-view tree end. index=" + i +
                    " nodes=" + browserCount + ".");
            }

            HostLog.Write("UIA embedded-browser roots end.");
        }

        private static void DumpInterestingPaths(AutomationElement root, string scope)
        {
            HostLog.Write("UIA interesting paths begin. scope=\"" + scope + "\"");
            var matches = new List<AutomationElement>();
            var visitedRuntimeIds = new HashSet<string>();
            FindInterestingDescendants(root, TreeWalker.RawViewWalker, 0, ref matches, visitedRuntimeIds,
                MaxEmbeddedBrowserDepth);

            HostLog.Write("UIA interesting paths count=" + matches.Count + " scope=\"" + scope + "\"");
            for (var i = 0; i < matches.Count; i++)
            {
                HostLog.Write("UIA interesting match scope=\"" + scope + "\" index=" + i + " " +
                    DescribeElement(matches[i]));
                HostLog.Write("UIA interesting path scope=\"" + scope + "\" index=" + i + " " +
                    BuildPath(root, matches[i]));
            }

            HostLog.Write("UIA interesting paths end. scope=\"" + scope + "\"");
        }

        private static void FindInterestingDescendants(AutomationElement element, TreeWalker walker, int depth,
            ref List<AutomationElement> matches, HashSet<string> visitedRuntimeIds)
        {
            FindInterestingDescendants(element, walker, depth, ref matches, visitedRuntimeIds, MaxRawDepth);
        }

        private static void FindInterestingDescendants(AutomationElement element, TreeWalker walker, int depth,
            ref List<AutomationElement> matches, HashSet<string> visitedRuntimeIds, int maxDepth)
        {
            if (element == null || depth > maxDepth || matches.Count >= MaxInterestingMatches)
            {
                return;
            }

            if (IsInterestingElement(element))
            {
                var runtimeId = GetRuntimeIdKey(element);
                if (visitedRuntimeIds.Add(runtimeId))
                {
                    matches.Add(element);
                }
            }

            AutomationElement child;
            try
            {
                child = walker.GetFirstChild(element);
            }
            catch
            {
                return;
            }

            while (child != null && matches.Count < MaxInterestingMatches)
            {
                FindInterestingDescendants(child, walker, depth + 1, ref matches, visitedRuntimeIds, maxDepth);
                try
                {
                    child = walker.GetNextSibling(child);
                }
                catch
                {
                    return;
                }
            }
        }

        private static void FindEmbeddedBrowserRoots(AutomationElement element, TreeWalker walker, int depth,
            List<AutomationElement> roots, HashSet<string> visitedRuntimeIds)
        {
            if (element == null || depth > MaxEmbeddedBrowserSearchDepth || roots.Count >= MaxEmbeddedBrowserRoots)
            {
                return;
            }

            if (IsEmbeddedBrowserRoot(element))
            {
                var runtimeId = GetRuntimeIdKey(element);
                if (visitedRuntimeIds.Add(runtimeId))
                {
                    roots.Add(element);
                }
            }

            AutomationElement child;
            try
            {
                child = walker.GetFirstChild(element);
            }
            catch
            {
                return;
            }

            while (child != null && roots.Count < MaxEmbeddedBrowserRoots)
            {
                FindEmbeddedBrowserRoots(child, walker, depth + 1, roots, visitedRuntimeIds);
                try
                {
                    child = walker.GetNextSibling(child);
                }
                catch
                {
                    return;
                }
            }
        }

        private static void FindProcessRoots(AutomationElement element, int processId, TreeWalker walker, int depth,
            List<AutomationElement> roots, HashSet<string> visitedRuntimeIds)
        {
            if (element == null || depth > MaxRawDepth || roots.Count >= MaxProcessRoots)
            {
                return;
            }

            var isProcessElement = TryGetElementProcessId(element, out var elementProcessId) &&
                elementProcessId == processId;
            if (isProcessElement)
            {
                var parentMatches = false;
                try
                {
                    var parent = walker.GetParent(element);
                    parentMatches = parent != null &&
                        TryGetElementProcessId(parent, out var parentProcessId) &&
                        parentProcessId == processId;
                }
                catch
                {
                    parentMatches = false;
                }

                if (!parentMatches)
                {
                    var runtimeId = GetRuntimeIdKey(element);
                    if (visitedRuntimeIds.Add(runtimeId))
                    {
                        roots.Add(element);
                    }
                }
            }

            AutomationElement child;
            try
            {
                child = walker.GetFirstChild(element);
            }
            catch
            {
                return;
            }

            while (child != null && roots.Count < MaxProcessRoots)
            {
                FindProcessRoots(child, processId, walker, depth + 1, roots, visitedRuntimeIds);
                try
                {
                    child = walker.GetNextSibling(child);
                }
                catch
                {
                    return;
                }
            }
        }

        private static void FindUnmuteButtons(AutomationElement element, TreeWalker walker, int depth,
            List<AutomationElement> matches, HashSet<string> visitedRuntimeIds)
        {
            if (element == null || depth > MaxEmbeddedBrowserDepth || matches.Count >= MaxUnmuteMatches)
            {
                return;
            }

            if (IsUnmuteButton(element))
            {
                var runtimeId = GetRuntimeIdKey(element);
                if (visitedRuntimeIds.Add(runtimeId))
                {
                    matches.Add(element);
                }
            }

            AutomationElement child;
            try
            {
                child = walker.GetFirstChild(element);
            }
            catch
            {
                return;
            }

            while (child != null && matches.Count < MaxUnmuteMatches)
            {
                FindUnmuteButtons(child, walker, depth + 1, matches, visitedRuntimeIds);
                try
                {
                    child = walker.GetNextSibling(child);
                }
                catch
                {
                    return;
                }
            }
        }

        private static bool IsUnmuteButton(AutomationElement element)
        {
            var name = Safe(() => element.Current.Name);
            var controlType = Safe(() => element.Current.ControlType.ProgrammaticName);
            return name.IndexOf("Unmute", StringComparison.OrdinalIgnoreCase) >= 0 &&
                controlType.IndexOf("Button", StringComparison.OrdinalIgnoreCase) >= 0;
        }

        private static bool TryGetElementProcessId(AutomationElement element, out int processId)
        {
            processId = 0;

            try
            {
                processId = element.Current.ProcessId;
                return processId > 0;
            }
            catch
            {
                processId = 0;
                return false;
            }
        }

        private static bool IsEmbeddedBrowserRoot(AutomationElement element)
        {
            var className = Safe(() => element.Current.ClassName);
            var frameworkId = Safe(() => element.Current.FrameworkId);
            var controlType = Safe(() => element.Current.ControlType.ProgrammaticName);

            return className.IndexOf("EmbeddedBrowserTabRootView", StringComparison.OrdinalIgnoreCase) >= 0 ||
                (frameworkId.IndexOf("Chrome", StringComparison.OrdinalIgnoreCase) >= 0 &&
                    controlType.IndexOf("Pane", StringComparison.OrdinalIgnoreCase) >= 0);
        }

        private static bool IsInterestingElement(AutomationElement element)
        {
            var automationId = Safe(() => element.Current.AutomationId);
            var name = Safe(() => element.Current.Name);
            var controlType = Safe(() => element.Current.ControlType.ProgrammaticName);

            return InterestingAutomationIds.Any(id =>
                    string.Equals(automationId, id, StringComparison.OrdinalIgnoreCase)) ||
                InterestingNameFragments.Any(fragment =>
                    name.IndexOf(fragment, StringComparison.OrdinalIgnoreCase) >= 0) ||
                (controlType.IndexOf("Button", StringComparison.OrdinalIgnoreCase) >= 0 &&
                    !string.IsNullOrWhiteSpace(name));
        }

        private static string BuildPath(AutomationElement root, AutomationElement element)
        {
            var path = new List<string>();
            var current = element;
            var safety = 0;

            while (current != null && safety++ < 80)
            {
                path.Insert(0, ShortElementLabel(current));
                if (SameRuntimeId(current, root))
                {
                    break;
                }

                try
                {
                    current = TreeWalker.RawViewWalker.GetParent(current);
                }
                catch (Exception ex)
                {
                    path.Insert(0, "parent unavailable:" + ex.Message);
                    break;
                }
            }

            return string.Join(" -> ", path);
        }

        private static string ShortElementLabel(AutomationElement element)
        {
            return "[" +
                "name=\"" + Safe(() => element.Current.Name) + "\"" +
                " automationId=\"" + Safe(() => element.Current.AutomationId) + "\"" +
                " type=\"" + Safe(() => element.Current.ControlType.ProgrammaticName) + "\"" +
                " pid=" + Safe(() => element.Current.ProcessId.ToString(CultureInfo.InvariantCulture)) +
                " hwnd=0x" + Safe(() => element.Current.NativeWindowHandle.ToString("X")) +
                "]";
        }

        private static bool SameRuntimeId(AutomationElement first, AutomationElement second)
        {
            return string.Equals(GetRuntimeIdKey(first), GetRuntimeIdKey(second), StringComparison.Ordinal);
        }

        private static string GetRuntimeIdKey(AutomationElement element)
        {
            try
            {
                return string.Join(".", element.GetRuntimeId() ?? Array.Empty<int>());
            }
            catch
            {
                return element.GetHashCode().ToString(CultureInfo.InvariantCulture);
            }
        }

        private static string DescribeElement(AutomationElement element)
        {
            var builder = new StringBuilder();
            builder.Append("name=\"").Append(Safe(() => element.Current.Name)).Append("\"");
            builder.Append(" automationId=\"").Append(Safe(() => element.Current.AutomationId)).Append("\"");
            builder.Append(" controlType=\"").Append(Safe(() => element.Current.ControlType.ProgrammaticName)).Append("\"");
            builder.Append(" class=\"").Append(Safe(() => element.Current.ClassName)).Append("\"");
            builder.Append(" framework=\"").Append(Safe(() => element.Current.FrameworkId)).Append("\"");
            builder.Append(" pid=").Append(Safe(() => element.Current.ProcessId.ToString(CultureInfo.InvariantCulture)));
            builder.Append(" hwnd=0x").Append(Safe(() => element.Current.NativeWindowHandle.ToString("X")));
            builder.Append(" enabled=").Append(Safe(() => element.Current.IsEnabled.ToString()));
            builder.Append(" offscreen=").Append(Safe(() => element.Current.IsOffscreen.ToString()));
            builder.Append(" rect=\"").Append(Safe(() => FormatRect(element.Current.BoundingRectangle))).Append("\"");
            builder.Append(" patterns=\"").Append(GetPatternNames(element)).Append("\"");
            return builder.ToString();
        }

        private static string GetPatternNames(AutomationElement element)
        {
            try
            {
                return string.Join(",", element.GetSupportedPatterns().Select(p => p.ProgrammaticName));
            }
            catch (Exception ex)
            {
                return "unavailable:" + ex.Message;
            }
        }

        private static string FormatRect(Rect rect)
        {
            return rect.X.ToString("0", CultureInfo.InvariantCulture) + "," +
                rect.Y.ToString("0", CultureInfo.InvariantCulture) + "," +
                rect.Width.ToString("0", CultureInfo.InvariantCulture) + "x" +
                rect.Height.ToString("0", CultureInfo.InvariantCulture);
        }

        private static string Safe(Func<string> read)
        {
            try
            {
                return read() ?? string.Empty;
            }
            catch (Exception ex)
            {
                return "unavailable:" + ex.Message;
            }
        }

        private static bool TryResolveWindow(string target, out IntPtr hwnd, out string resolvedBy)
        {
            hwnd = IntPtr.Zero;
            resolvedBy = string.Empty;
            target = (target ?? string.Empty).Trim();
            if (string.IsNullOrWhiteSpace(target))
            {
                return false;
            }

            if (target.StartsWith("hwnd:", StringComparison.OrdinalIgnoreCase))
            {
                return TryParseHwnd(target.Substring(5), out hwnd, out resolvedBy);
            }

            if (target.StartsWith("pid:", StringComparison.OrdinalIgnoreCase))
            {
                return TryResolvePid(target.Substring(4), out hwnd, out resolvedBy);
            }

            if (target.StartsWith("title:", StringComparison.OrdinalIgnoreCase))
            {
                return TryResolveTitle(target.Substring(6), out hwnd, out resolvedBy);
            }

            if (target.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            {
                return TryParseHwnd(target, out hwnd, out resolvedBy);
            }

            if (int.TryParse(target, NumberStyles.Integer, CultureInfo.InvariantCulture, out var number))
            {
                hwnd = new IntPtr(number);
                if (IsWindow(hwnd))
                {
                    resolvedBy = "decimal hwnd";
                    return true;
                }

                return TryResolvePid(target, out hwnd, out resolvedBy);
            }

            return TryResolveTitle(target, out hwnd, out resolvedBy);
        }

        private static bool TryParseHwnd(string value, out IntPtr hwnd, out string resolvedBy)
        {
            hwnd = IntPtr.Zero;
            resolvedBy = string.Empty;
            value = (value ?? string.Empty).Trim();
            if (value.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            {
                value = value.Substring(2);
            }

            if (!long.TryParse(value, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var numeric))
            {
                return false;
            }

            hwnd = new IntPtr(numeric);
            if (!IsWindow(hwnd))
            {
                return false;
            }

            resolvedBy = "hwnd";
            return true;
        }

        private static bool TryResolvePid(string value, out IntPtr hwnd, out string resolvedBy)
        {
            hwnd = IntPtr.Zero;
            resolvedBy = string.Empty;
            if (!int.TryParse((value ?? string.Empty).Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture,
                    out var pid))
            {
                return false;
            }

            var window = GetTopLevelWindows()
                .FirstOrDefault(w => w.ProcessId == pid && !string.IsNullOrWhiteSpace(w.Title));
            if (window == null)
            {
                window = GetTopLevelWindows().FirstOrDefault(w => w.ProcessId == pid);
            }

            if (window == null)
            {
                window = FindWindowHostingProcess(pid);
                if (window == null)
                {
                    return false;
                }

                hwnd = window.Hwnd;
                resolvedBy = "hosted pid";
                return true;
            }

            hwnd = window.Hwnd;
            resolvedBy = "pid";
            return true;
        }

        private static WindowInfo FindWindowHostingProcess(int processId)
        {
            foreach (var window in GetTopLevelWindows().Where(w => !string.IsNullOrWhiteSpace(w.Title)))
            {
                try
                {
                    var root = AutomationElement.FromHandle(window.Hwnd);
                    if (root != null && ContainsProcessId(root, processId, TreeWalker.RawViewWalker, 0, 0))
                    {
                        return window;
                    }
                }
                catch
                {
                }
            }

            return null;
        }

        private static bool ContainsProcessId(AutomationElement element, int processId, TreeWalker walker, int depth,
            int nodeCount)
        {
            if (element == null || depth > MaxEmbeddedBrowserSearchDepth || nodeCount > MaxRawNodes)
            {
                return false;
            }

            if (TryGetElementProcessId(element, out var elementProcessId) && elementProcessId == processId)
            {
                return true;
            }

            AutomationElement child;
            try
            {
                child = walker.GetFirstChild(element);
            }
            catch
            {
                return false;
            }

            while (child != null)
            {
                nodeCount++;
                if (ContainsProcessId(child, processId, walker, depth + 1, nodeCount))
                {
                    return true;
                }

                try
                {
                    child = walker.GetNextSibling(child);
                }
                catch
                {
                    return false;
                }
            }

            return false;
        }

        private static int? TryGetRequestedProcessId(string target, string resolvedBy)
        {
            target = (target ?? string.Empty).Trim();
            if (target.StartsWith("pid:", StringComparison.OrdinalIgnoreCase))
            {
                target = target.Substring(4).Trim();
            }
            else if (target.StartsWith("hwnd:", StringComparison.OrdinalIgnoreCase) ||
                     target.StartsWith("title:", StringComparison.OrdinalIgnoreCase) ||
                     target.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            {
                return null;
            }
            else if (!string.Equals(resolvedBy, "pid", StringComparison.OrdinalIgnoreCase) &&
                     !string.Equals(resolvedBy, "hosted pid", StringComparison.OrdinalIgnoreCase))
            {
                return null;
            }

            if (int.TryParse(target, NumberStyles.Integer, CultureInfo.InvariantCulture, out var processId) &&
                processId > 0)
            {
                return processId;
            }

            return null;
        }

        private static bool TryResolveTitle(string value, out IntPtr hwnd, out string resolvedBy)
        {
            hwnd = IntPtr.Zero;
            resolvedBy = string.Empty;
            value = (value ?? string.Empty).Trim();
            if (string.IsNullOrWhiteSpace(value))
            {
                return false;
            }

            var window = GetTopLevelWindows()
                .FirstOrDefault(w => w.Title.IndexOf(value, StringComparison.OrdinalIgnoreCase) >= 0);
            if (window == null)
            {
                return false;
            }

            hwnd = window.Hwnd;
            resolvedBy = "title";
            return true;
        }

        private static bool WindowMatchesFilter(WindowInfo window, string filter)
        {
            filter = filter.Trim();
            return window.Title.IndexOf(filter, StringComparison.OrdinalIgnoreCase) >= 0 ||
                window.ProcessName.IndexOf(filter, StringComparison.OrdinalIgnoreCase) >= 0 ||
                window.ProcessId.ToString(CultureInfo.InvariantCulture).Equals(filter, StringComparison.OrdinalIgnoreCase) ||
                ("0x" + window.Hwnd.ToInt64().ToString("X")).Equals(filter, StringComparison.OrdinalIgnoreCase);
        }

        private static List<WindowInfo> GetTopLevelWindows()
        {
            var windows = new List<WindowInfo>();
            EnumWindows((hwnd, lParam) =>
            {
                if (!IsWindowVisible(hwnd))
                {
                    return true;
                }

                GetWindowThreadProcessId(hwnd, out var processId);
                var title = GetWindowTitle(hwnd);
                var processName = GetProcessName(processId);
                windows.Add(new WindowInfo(hwnd, processId, processName, title));
                return true;
            }, IntPtr.Zero);
            return windows;
        }

        private static string GetWindowTitle(IntPtr hwnd)
        {
            var length = GetWindowTextLength(hwnd);
            if (length <= 0)
            {
                return string.Empty;
            }

            var buffer = new char[length + 1];
            var copied = GetWindowText(hwnd, buffer, buffer.Length);
            return copied <= 0 ? string.Empty : new string(buffer, 0, copied);
        }

        private static string GetProcessName(int processId)
        {
            try
            {
                return Process.GetProcessById(processId).ProcessName;
            }
            catch
            {
                return string.Empty;
            }
        }

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern bool IsWindowVisible(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern bool IsWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out int lpdwProcessId);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowText(IntPtr hWnd, [Out] char[] lpString, int nMaxCount);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowTextLength(IntPtr hWnd);

        private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

        private sealed class WindowInfo
        {
            public WindowInfo(IntPtr hwnd, int processId, string processName, string title)
            {
                Hwnd = hwnd;
                ProcessId = processId;
                ProcessName = processName ?? string.Empty;
                Title = title ?? string.Empty;
            }

            public IntPtr Hwnd { get; }
            public int ProcessId { get; }
            public string ProcessName { get; }
            public string Title { get; }
        }
    }
}
