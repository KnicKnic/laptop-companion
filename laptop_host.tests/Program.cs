using System;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Automation;
using X3LaptopCompanion;

internal static class Program
{
    private static int assertions;

    [STAThread]
    private static void Main(string[] args)
    {
        var cases = new (string Description, CompanionTriState Expected)[]
        {
            ("Raise your hand", CompanionTriState.Off),
            ("Lower your hand", CompanionTriState.On),
            ("Raise your hand. Has context menu", CompanionTriState.Off),
            ("Lower your hand. Has context menu", CompanionTriState.On),
            ("Raise your hand (Ctrl+Shift+K)", CompanionTriState.Off),
            ("Lower your hand (Ctrl+Shift+K)", CompanionTriState.On),
            ("Raise your hand.Has context menu", CompanionTriState.Off),
            ("Lower your hand\nHas context menu", CompanionTriState.On),
            (" \tLOWER YOUR HAND. Has context menu\r\n", CompanionTriState.On),
            ("raise your hand", CompanionTriState.Off),
            (null, CompanionTriState.Unknown),
            ("", CompanionTriState.Unknown),
            (" \r\n\t", CompanionTriState.Unknown),
            ("Raise", CompanionTriState.Unknown),
            ("Lower", CompanionTriState.Unknown),
            ("Has context menu", CompanionTriState.Unknown),
            ("Lower your hands", CompanionTriState.Unknown),
            ("Raise your handler", CompanionTriState.Unknown),
            ("Cannot lower your hand", CompanionTriState.Unknown),
            ("Main levee", CompanionTriState.Unknown)
        };
        foreach (var test in cases)
        {
            Assert(TeamsController.ParseHandState(test.Description) == test.Expected,
                "Unexpected hand state for description: " + (test.Description ?? "<null>"));
        }

        AssertThrows<ArgumentException>(() => NativeUiAutomation.ReadFullDescription(IntPtr.Zero, new[] { 1 }));
        AssertThrows<ArgumentException>(() => NativeUiAutomation.ReadFullDescription(new IntPtr(1), null));
        AssertThrows<ArgumentException>(() => NativeUiAutomation.ReadFullDescription(new IntPtr(1), Array.Empty<int>()));

        if (args.Length != 0)
        {
            if (args.Length != 3 || args[0] != "--live")
            {
                throw new ArgumentException("Usage: [--live <Teams audio PID> <Teams window PID>]");
            }

            var audioProcessId = int.Parse(args[1]);
            var targetProcessId = int.Parse(args[2]);
            // Match the MTA worker thread used by the host. These checks only read UIA.
            Task.Run(() => CheckLiveMeeting(audioProcessId, targetProcessId)).GetAwaiter().GetResult();
        }

        Console.WriteLine("Passed " + assertions + " assertions. No Teams controls were invoked.");
    }

    private static void CheckLiveMeeting(int audioProcessId, int targetProcessId)
    {
        var windows = AutomationElement.RootElement.FindAll(TreeScope.Children,
            new PropertyCondition(AutomationElement.ProcessIdProperty, targetProcessId));
        var handCondition = new AndCondition(
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button),
            new PropertyCondition(AutomationElement.AutomationIdProperty, "raisehands-button"));
        var hwnd = IntPtr.Zero;
        AutomationElement button = null;
        foreach (AutomationElement window in windows)
        {
            button = window.FindFirst(TreeScope.Descendants, handCondition);
            if (button == null) continue;
            hwnd = new IntPtr(window.Current.NativeWindowHandle);
            break;
        }

        Assert(button != null, "Teams hand button was not found.");
        Assert(hwnd != IntPtr.Zero, "Teams meeting window is unavailable.");

        var runtimeId = button.GetRuntimeId();
        var description = NativeUiAutomation.ReadFullDescription(hwnd, runtimeId);
        Assert(TeamsController.ParseHandState(description) != CompanionTriState.Unknown,
            "Live hand description was not recognized: " + description);
        Console.WriteLine("Live button name=\"" + button.Current.Name + "\" description=\"" + description + "\"");

        AssertThrows<ElementNotAvailableException>(() =>
            NativeUiAutomation.ReadFullDescription(hwnd, new[] { int.MinValue, int.MinValue }));
        Assert(!runtimeId.SequenceEqual(new[] { int.MinValue, int.MinValue }), "Invalid runtime ID fixture.");

        var controller = new TeamsController();
        for (var i = 0; i < 3; i++)
        {
            var snapshot = controller.GetMeetingSnapshot(new[] { audioProcessId }, i == 0 ? targetProcessId : null);
            Assert(snapshot.MeetingDetected, "Live meeting was not detected.");
            Assert(snapshot.Hand != CompanionTriState.Unknown, "Snapshot did not read a real hand state.");
            Assert(!snapshot.HandLocked, "Readable live hand state was unexpectedly locked.");
            Assert(snapshot.Detail.Contains("handDescription=\""), "Snapshot omitted its hand-description diagnostic.");
            Assert(snapshot.Detail.Contains("hwnd=0x" + hwnd.ToInt64().ToString("X") + " "),
                "Snapshot selected a compact/sharing window instead of the window containing the hand control.");
            Console.WriteLine("Snapshot " + (i + 1) + ": hand=" + snapshot.Hand +
                " mic=" + snapshot.Microphone + " camera=" + snapshot.Camera);
        }
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
        assertions++;
    }

    private static void AssertThrows<T>(Action action) where T : Exception
    {
        try
        {
            action();
        }
        catch (T)
        {
            assertions++;
            return;
        }

        throw new InvalidOperationException("Expected " + typeof(T).Name + ".");
    }
}
