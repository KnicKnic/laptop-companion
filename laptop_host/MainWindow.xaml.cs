using System.ComponentModel;
using System.Diagnostics;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;

namespace X3LaptopCompanion
{
    public partial class MainWindow : Window, INotifyPropertyChanged
    {
        private readonly TeamsController teamsController = new TeamsController();
        private readonly MediaStatusSensor mediaStatusSensor = new MediaStatusSensor();
        private readonly UiaWindowDumper uiaWindowDumper = new UiaWindowDumper();
        private readonly CompanionConnectionService connectionService = new CompanionConnectionService();
        private readonly DispatcherTimer statusTimer = new DispatcherTimer();

        private string connectionText = "Disconnected";
        private string teamsText = "Waiting for Teams";
        private string meetingText = "Unknown";
        private string microphoneText = "Unknown";
        private string cameraText = "Unknown";
        private string handText = "Unknown";
        private string buttonProtocolText = "No pending button";
        private string participationText = "No participation pulses";
        private bool isTestMode;
        private string testModeText = "Off";
        private bool testTeamsDetected = true;
        private bool testMeetingDetected;
        private CompanionTriState testMicrophone = CompanionTriState.Unknown;
        private CompanionTriState testCamera = CompanionTriState.Unknown;
        private CompanionTriState testHand = CompanionTriState.Unknown;
        private string testMessage = "Test status";
        private string testMeetingName = "Test Meeting";
        private string testTeamsToggleText = "Detected";
        private string testMeetingToggleText = "No meeting";
        private string testMicrophoneToggleText = "Unknown";
        private string testCameraToggleText = "Unknown";
        private string testHandToggleText = "Unknown";
        private bool isTeamsDryRun;
        private string teamsModeText = "Live Teams";
        private string commandTargetProcessIdText = string.Empty;
        private string windowDumpTargetText = string.Empty;
        private readonly System.Collections.Generic.Dictionary<CompanionButton, ushort> lastButtonSequences =
            new System.Collections.Generic.Dictionary<CompanionButton, ushort>();
        private ushort simulatedButtonSequence = 0x7E00;
        private CompanionButton? pendingButton;
        private ushort pendingButtonCounter;
        private CompanionTriState? pendingButtonExpectedState;
        private ushort microphoneStateCounter;
        private ushort cameraStateCounter;
        private ushort handStateCounter;
        private bool wasBleConnected;
        private bool hostStateWriteInFlight;
        private bool isExiting;
        private bool statusRefreshInFlight;
        private bool teamsCommandRefreshInFlight;
        private HostStatePayload lastSentHostState;
        private HostStatePayload pendingHostState;
        private string pendingHostStateReason;
        private string detailText =
            "Open Laptop Companion on the X3 Home screen, then pair/connect over BLE once the firmware service is wired in.";

        private sealed class HostStatePayload
        {
            public HostStatePayload(bool teamsDetected, bool meetingDetected, string meetingName,
                CompanionTriState microphone, CompanionTriState camera, CompanionTriState hand, string message,
                ushort teamsCounter, ushort meetingCounter, ushort microphoneCounter, ushort cameraCounter,
                ushort handCounter)
            {
                TeamsDetected = teamsDetected;
                MeetingDetected = meetingDetected;
                MeetingName = string.IsNullOrWhiteSpace(meetingName) ? string.Empty : meetingName;
                Microphone = microphone;
                Camera = camera;
                Hand = hand;
                Message = string.IsNullOrWhiteSpace(message) ? string.Empty : message;
                TeamsCounter = teamsCounter;
                MeetingCounter = meetingCounter;
                MicrophoneCounter = microphoneCounter;
                CameraCounter = cameraCounter;
                HandCounter = handCounter;
            }

            public bool TeamsDetected { get; }
            public bool MeetingDetected { get; }
            public string MeetingName { get; }
            public CompanionTriState Microphone { get; }
            public CompanionTriState Camera { get; }
            public CompanionTriState Hand { get; }
            public string Message { get; }
            public ushort TeamsCounter { get; }
            public ushort MeetingCounter { get; }
            public ushort MicrophoneCounter { get; }
            public ushort CameraCounter { get; }
            public ushort HandCounter { get; }

            public ushort CounterFor(CompanionButton button)
            {
                if (button == CompanionButton.ToggleMute)
                {
                    return MicrophoneCounter;
                }

                if (button == CompanionButton.ToggleHand)
                {
                    return HandCounter;
                }

                return CameraCounter;
            }

            public bool SameAs(HostStatePayload other)
            {
                return other != null &&
                    TeamsDetected == other.TeamsDetected &&
                    MeetingDetected == other.MeetingDetected &&
                    string.Equals(MeetingName, other.MeetingName, System.StringComparison.Ordinal) &&
                    Microphone == other.Microphone &&
                    Camera == other.Camera &&
                    Hand == other.Hand &&
                    string.Equals(Message, other.Message, System.StringComparison.Ordinal) &&
                    TeamsCounter == other.TeamsCounter &&
                    MeetingCounter == other.MeetingCounter &&
                    MicrophoneCounter == other.MicrophoneCounter &&
                    CameraCounter == other.CameraCounter &&
                    HandCounter == other.HandCounter;
            }
        }

        public MainWindow()
        {
            InitializeComponent();
            DataContext = this;
            HostLog.Write("Main window created.");
            connectionService.StatusChanged += OnConnectionStatusChanged;
            connectionService.ButtonEventReceived += OnButtonEventReceived;
            connectionService.ParticipationEventReceived += OnParticipationEventReceived;
            statusTimer.Interval = System.TimeSpan.FromSeconds(2);
            statusTimer.Tick += OnStatusTimerTick;
            Loaded += OnLoaded;
            Closing += OnClosing;
        }

        public event PropertyChangedEventHandler PropertyChanged;

        public string ConnectionText
        {
            get { return connectionText; }
            private set { SetField(ref connectionText, value, nameof(ConnectionText)); }
        }

        public string TeamsText
        {
            get { return teamsText; }
            private set { SetField(ref teamsText, value, nameof(TeamsText)); }
        }

        public string MeetingText
        {
            get { return meetingText; }
            private set { SetField(ref meetingText, value, nameof(MeetingText)); }
        }

        public string MicrophoneText
        {
            get { return microphoneText; }
            private set { SetField(ref microphoneText, value, nameof(MicrophoneText)); }
        }

        public string CameraText
        {
            get { return cameraText; }
            private set { SetField(ref cameraText, value, nameof(CameraText)); }
        }

        public string HandText
        {
            get { return handText; }
            private set { SetField(ref handText, value, nameof(HandText)); }
        }

        public string ButtonProtocolText
        {
            get { return buttonProtocolText; }
            private set { SetField(ref buttonProtocolText, value, nameof(ButtonProtocolText)); }
        }

        public string ParticipationText
        {
            get { return participationText; }
            private set { SetField(ref participationText, value, nameof(ParticipationText)); }
        }

        public bool IsTestMode
        {
            get { return isTestMode; }
            set
            {
                if (SetField(ref isTestMode, value, nameof(IsTestMode)))
                {
                    ApplyTestMode();
                }
            }
        }

        public string TestModeText
        {
            get { return testModeText; }
            private set { SetField(ref testModeText, value, nameof(TestModeText)); }
        }

        public bool TestTeamsDetected
        {
            get { return testTeamsDetected; }
            set
            {
                if (SetField(ref testTeamsDetected, value, nameof(TestTeamsDetected)))
                {
                    TestTeamsToggleText = testTeamsDetected ? "Detected" : "Not detected";
                    OnTestStatusChanged("teams");
                }
            }
        }

        public string TestMessage
        {
            get { return testMessage; }
            set
            {
                if (SetField(ref testMessage, value, nameof(TestMessage)) && IsTestMode)
                {
                    DetailText = "Test message updated. Press Send Test Status to push it to the X3.";
                }
            }
        }

        public string TestTeamsToggleText
        {
            get { return testTeamsToggleText; }
            private set { SetField(ref testTeamsToggleText, value, nameof(TestTeamsToggleText)); }
        }

        public bool TestMeetingDetected
        {
            get { return testMeetingDetected; }
            set
            {
                if (SetField(ref testMeetingDetected, value, nameof(TestMeetingDetected)))
                {
                    TestMeetingToggleText = testMeetingDetected ? "In meeting" : "No meeting";
                    OnTestStatusChanged("meeting");
                }
            }
        }

        public string TestMeetingName
        {
            get { return testMeetingName; }
            set
            {
                if (SetField(ref testMeetingName, value, nameof(TestMeetingName)) && IsTestMode)
                {
                    OnTestStatusChanged("meeting name");
                }
            }
        }

        public string TestMeetingToggleText
        {
            get { return testMeetingToggleText; }
            private set { SetField(ref testMeetingToggleText, value, nameof(TestMeetingToggleText)); }
        }

        public string TestMicrophoneToggleText
        {
            get { return testMicrophoneToggleText; }
            private set { SetField(ref testMicrophoneToggleText, value, nameof(TestMicrophoneToggleText)); }
        }

        public string TestCameraToggleText
        {
            get { return testCameraToggleText; }
            private set { SetField(ref testCameraToggleText, value, nameof(TestCameraToggleText)); }
        }

        public string TestHandToggleText
        {
            get { return testHandToggleText; }
            private set { SetField(ref testHandToggleText, value, nameof(TestHandToggleText)); }
        }

        public bool IsTeamsDryRun
        {
            get { return isTeamsDryRun; }
            set
            {
                if (SetField(ref isTeamsDryRun, value, nameof(IsTeamsDryRun)))
                {
                    ApplyTeamsDryRun();
                }
            }
        }

        public string TeamsModeText
        {
            get { return teamsModeText; }
            private set { SetField(ref teamsModeText, value, nameof(TeamsModeText)); }
        }

        public string CommandTargetProcessIdText
        {
            get { return commandTargetProcessIdText; }
            set
            {
                if (SetField(ref commandTargetProcessIdText, value, nameof(CommandTargetProcessIdText)))
                {
                    HostLog.Write("Command target PID text changed. value=\"" + value + "\"");
                }
            }
        }

        public string WindowDumpTargetText
        {
            get { return windowDumpTargetText; }
            set
            {
                if (SetField(ref windowDumpTargetText, value, nameof(WindowDumpTargetText)))
                {
                    HostLog.Write("UIA dump target text changed. value=\"" + value + "\"");
                }
            }
        }

        public string DetailText
        {
            get { return detailText; }
            private set { SetField(ref detailText, value, nameof(DetailText)); }
        }

        public void ToggleMuteFromUi()
        {
            SendTeamsCommandFromUi(TeamsCommand.ToggleMute);
        }

        public void SimulateX3MutePress()
        {
            HostLog.Write("Simulate X3 mute press requested.");
            if (!IsTestMode)
            {
                DetailText = "Enable test mode before simulating X3 commands.";
                return;
            }

            simulatedButtonSequence++;
            simulatedButtonSequence = NormalizeProtocolCounter(simulatedButtonSequence);
            OnButtonEventReceived(this, new CompanionButtonEvent(CompanionButton.ToggleMute,
                CompanionButtonAction.Released, simulatedButtonSequence, 0));
        }

        public void SendTestStatus()
        {
            HostLog.Write("Test status requested.");
            if (IsTestMode)
            {
                SendTestHostStatus("manual");
                return;
            }

            if (!IsTeamsDryRun)
            {
                RefreshTeamsPresence();
            }
            else
            {
                TeamsText = "Dry run";
            }

            MicrophoneText = "Muted";
            CameraText = "Off";
            if (IsTestMode)
            {
                DetailText = "Test status shown locally. BLE writes are disabled while test mode is on.";
                return;
            }

            DetailText = "Test status queued for BLE if it changed.";
            QueueHostStatusIfChanged(true, true, "BLE test meeting", CompanionTriState.Off, CompanionTriState.Off,
                CompanionTriState.Off, "BLE test status",
                "manual test status");
        }

        private void OnLoaded(object sender, RoutedEventArgs e)
        {
            HostLog.Write("Main window loaded. TestMode=" + IsTestMode);
            RefreshTeamsPresence();
            connectionService.Start();
            statusTimer.Start();
        }

        private void OnClosing(object sender, CancelEventArgs e)
        {
            if (isExiting || Application.Current.Dispatcher.HasShutdownStarted)
            {
                HostLog.Write("Main window close requested for app exit.");
                StopServicesForExit();
                return;
            }

            HostLog.Write("Main window close requested; hiding to tray.");
            e.Cancel = true;
            Hide();
        }

        public void ExitApplication()
        {
            if (isExiting)
            {
                return;
            }

            HostLog.Write("Application exit requested.");
            isExiting = true;
            StopServicesForExit();
            Application.Current.Shutdown();
        }

        private void ToggleMute_Click(object sender, RoutedEventArgs e)
        {
            ToggleMuteFromUi();
        }

        private void SimulateX3Mute_Click(object sender, RoutedEventArgs e)
        {
            SimulateX3MutePress();
        }

        private void SendTestStatus_Click(object sender, RoutedEventArgs e)
        {
            SendTestStatus();
        }

        private void OpenLog_Click(object sender, RoutedEventArgs e)
        {
            OpenLog();
        }

        private void ToggleSpeaker_Click(object sender, RoutedEventArgs e)
        {
            SendTeamsCommandFromUi(TeamsCommand.ToggleSpeaker);
        }

        private void ToggleHand_Click(object sender, RoutedEventArgs e)
        {
            SendTeamsCommandFromUi(TeamsCommand.ToggleHand);
        }

        private void ToggleVideo_Click(object sender, RoutedEventArgs e)
        {
            SendTeamsCommandFromUi(TeamsCommand.ToggleVideo);
        }

        private void DumpWindow_Click(object sender, RoutedEventArgs e)
        {
            DumpWindow();
        }

        private void ListWindows_Click(object sender, RoutedEventArgs e)
        {
            ListWindows();
        }

        private void CycleTestMicrophone_Click(object sender, RoutedEventArgs e)
        {
            testMicrophone = NextTriState(testMicrophone);
            TestMicrophoneToggleText = TriStateText(testMicrophone, "Muted", "Live");
            OnTestStatusChanged("microphone");
        }

        private void CycleTestCamera_Click(object sender, RoutedEventArgs e)
        {
            testCamera = NextTriState(testCamera);
            TestCameraToggleText = TriStateText(testCamera, "Off", "Active");
            OnTestStatusChanged("camera");
        }

        private void CycleTestHand_Click(object sender, RoutedEventArgs e)
        {
            testHand = NextTriState(testHand);
            TestHandToggleText = TriStateText(testHand, "Lowered", "Raised");
            OnTestStatusChanged("hand");
        }

        private void Hide_Click(object sender, RoutedEventArgs e)
        {
            Hide();
        }

        public void OpenLog()
        {
            HostLog.Write("Open log requested.");
            var logPath = HostLog.LogPath;
            Process.Start(new ProcessStartInfo
            {
                FileName = "explorer.exe",
                Arguments = "/select,\"" + logPath + "\"",
                UseShellExecute = true
            });
        }

        private void DumpWindow()
        {
            var target = WindowDumpTargetText;
            if (string.IsNullOrWhiteSpace(target))
            {
                DetailText = "Enter a window target before dumping.";
                HostLog.Write("UIA dump skipped; target is blank.");
                return;
            }

            DetailText = "UIA dump queued. Use Open Log to inspect it.";
            HostLog.Write("UIA dump requested. target=\"" + target + "\"");
            _ = Task.Run(() =>
            {
                var dumped = uiaWindowDumper.DumpWindow(target);
                Dispatcher.Invoke(() =>
                {
                    DetailText = dumped
                        ? "UIA dump written to the log."
                        : "UIA dump target was not found. Use List Windows to discover a target.";
                });
            });
        }

        private void ListWindows()
        {
            var filter = WindowDumpTargetText;
            DetailText = "Window list queued. Use Open Log to inspect it.";
            HostLog.Write("UIA window list requested. filter=\"" + filter + "\"");
            _ = Task.Run(() =>
            {
                uiaWindowDumper.ListWindows(filter);
                Dispatcher.Invoke(() =>
                {
                    DetailText = "Window list written to the log.";
                });
            });
        }

        private void RefreshTeamsPresence()
        {
            if (IsTeamsDryRun)
            {
                TeamsText = "Dry run";
                MeetingText = "No";
                CameraText = "Unknown";
                MicrophoneText = "Unknown";
                HandText = "Unknown";
                return;
            }

            var snapshot = ReadTeamsMeetingSnapshot();
            ApplyTeamsSnapshotToUi(snapshot);
        }

        private void ApplyTeamsSnapshotToUi(TeamsMeetingSnapshot snapshot)
        {
            TeamsText = TeamsTextForSnapshot(snapshot);
            MeetingText = snapshot.MeetingDetected
                ? (string.IsNullOrWhiteSpace(snapshot.MeetingName) ? "Yes" : snapshot.MeetingName)
                : "No";
            MicrophoneText = TriStateText(snapshot.Microphone, "Muted", "Live");
            CameraText = TriStateText(snapshot.Camera, "Off", "Active");
            HandText = TriStateText(snapshot.Hand, "Lowered", "Raised");
        }

        private void OnStatusTimerTick(object sender, System.EventArgs e)
        {
            if (isExiting)
            {
                return;
            }

            if (IsTestMode)
            {
                ApplyTestStatusToUi();
                SendTestHostStatus("timer");
                return;
            }

            if (IsTeamsDryRun)
            {
                RefreshTeamsPresence();
                SendCurrentHostStatus();
                return;
            }

            if (teamsCommandRefreshInFlight)
            {
                HostLog.Write("Live Teams status refresh skipped; Teams command refresh is in flight.");
                return;
            }

            if (statusRefreshInFlight)
            {
                HostLog.Write("Live Teams status refresh skipped; previous refresh is still running.");
                return;
            }

            statusRefreshInFlight = true;
            _ = RefreshLiveTeamsStatusAsync(ParseCommandTargetProcessId());
        }

        private async Task RefreshLiveTeamsStatusAsync(int? explicitTargetProcessId)
        {
            try
            {
                var snapshot = await Task.Run(() => ReadTeamsMeetingSnapshot(true, explicitTargetProcessId));
                if (isExiting || Dispatcher.HasShutdownStarted)
                {
                    return;
                }

                await Dispatcher.InvokeAsync(() =>
                {
                    if (isExiting)
                    {
                        return;
                    }

                    if (teamsCommandRefreshInFlight)
                    {
                        HostLog.Write("Live Teams status refresh result discarded; Teams command refresh is in flight.");
                        return;
                    }

                    ApplyTeamsSnapshotToUi(snapshot);
                    QueueHostStatusIfChanged(snapshot.TeamsDetected, snapshot.MeetingDetected, snapshot.MeetingName,
                        snapshot.Microphone, snapshot.Camera, snapshot.Hand, StatusMessageForSnapshot(snapshot),
                        "current");
                });
            }
            catch (System.Exception ex)
            {
                HostLog.Write("Live Teams status refresh failed.", ex);
            }
            finally
            {
                statusRefreshInFlight = false;
            }
        }

        private void OnConnectionStatusChanged(object sender, CompanionConnectionStatus status)
        {
            if (isExiting || Dispatcher.HasShutdownStarted)
            {
                return;
            }

            Dispatcher.BeginInvoke(new System.Action(() =>
            {
                if (isExiting)
                {
                    return;
                }

                HostLog.Write("UI status received. connected=" + status.IsConnected + " message=" + status.Message);
                ConnectionText = status.IsConnected ? "Connected" : "Disconnected";
                DetailText = status.Message;
                lastButtonSequences.Clear();
                var reconnected = status.IsConnected && !wasBleConnected;
                wasBleConnected = status.IsConnected;
                if (!status.IsConnected)
                {
                    ResetHostStateWriteCache();
                    return;
                }

                if (reconnected)
                {
                    SendCurrentHostStatus(force: true);
                }
            }));
        }

        private void OnButtonEventReceived(object sender, CompanionButtonEvent buttonEvent)
        {
            HostLog.Write("UI button event received. button=" + buttonEvent.Button +
                " action=" + buttonEvent.Action + " seq=" + buttonEvent.Sequence +
                " deviceUptimeMs=" + buttonEvent.DeviceUptimeMs);
            if (buttonEvent.Action != CompanionButtonAction.Released)
            {
                return;
            }

            if (lastButtonSequences.TryGetValue(buttonEvent.Button, out var previousSequence) &&
                previousSequence == buttonEvent.Sequence)
            {
                HostLog.Write("Duplicate button event ignored. button=" + buttonEvent.Button +
                    " seq=" + buttonEvent.Sequence);
                return;
            }

            lastButtonSequences[buttonEvent.Button] = buttonEvent.Sequence;
            pendingButton = buttonEvent.Button;
            pendingButtonCounter = buttonEvent.Sequence;
            pendingButtonExpectedState = null;
            if (isExiting || Dispatcher.HasShutdownStarted)
            {
                return;
            }

            Dispatcher.BeginInvoke(new System.Action(() =>
            {
                if (isExiting)
                {
                    return;
                }

                ButtonProtocolText = ButtonName(buttonEvent.Button) + " #" + buttonEvent.Sequence + " pressed";
                TeamsCommand? command = null;
                switch (buttonEvent.Button)
                {
                    case CompanionButton.ToggleMute:
                        command = TeamsCommand.ToggleMute;
                        break;
                    case CompanionButton.ToggleHand:
                        command = TeamsCommand.ToggleHand;
                        break;
                    case CompanionButton.ToggleCamera:
                        command = TeamsCommand.ToggleVideo;
                        break;
                }

                if (command.HasValue)
                {
                    HostLog.Write("BLE button dispatching Teams command. button=" + buttonEvent.Button +
                        " command=" + TeamsController.CommandName(command.Value));
                    SendTeamsCommandFromUi(command.Value);
                }
            }));
        }

        private void OnParticipationEventReceived(object sender, CompanionParticipationEvent participationEvent)
        {
            if (isExiting || Dispatcher.HasShutdownStarted)
            {
                return;
            }

            Dispatcher.InvokeAsync(new System.Action(() =>
            {
                if (!isExiting)
                {
                    ParticipationText = "#" + participationEvent.Counter;
                }
            }), DispatcherPriority.Send);
        }

        private void ApplyTestMode()
        {
            HostLog.Write("Apply test mode. enabled=" + IsTestMode);
            if (IsTestMode)
            {
                TestModeText = "On";
                ApplyTestStatusToUi();
                DetailText = "Test mode is active. BLE stays connected and sends the simulated status to the X3.";
                connectionService.Start();
                SendTestHostStatus("enabled");
                return;
            }

            TestModeText = "Off";
            ConnectionText = "Disconnected";
            DetailText = "Test mode disabled. Scanning for the X3 companion service.";
            connectionService.Start();
        }

        private void ApplyTeamsDryRun()
        {
            HostLog.Write("Apply Teams dry run. enabled=" + IsTeamsDryRun);
            TeamsModeText = IsTeamsDryRun ? "Dry run" : "Live Teams";
            RefreshTeamsPresence();
            DetailText = IsTeamsDryRun
                ? "Teams dry run is active. BLE stays connected, but Teams will not be focused or controlled."
                : "Teams dry run disabled. X3 mute commands will control Teams.";
            if (!IsTestMode)
            {
                SendCurrentHostStatus();
            }
        }

        private void SendCurrentHostStatus(bool force = false)
        {
            if (IsTestMode)
            {
                SendTestHostStatus("current", force);
                return;
            }

            if (IsTeamsDryRun)
            {
                QueueHostStatusIfChanged(true, false, string.Empty, CompanionTriState.Unknown, CompanionTriState.Unknown,
                    CompanionTriState.Unknown, "Teams dry run", "current dry-run", force);
                return;
            }

            var snapshot = ReadTeamsMeetingSnapshot();
            QueueHostStatusIfChanged(snapshot.TeamsDetected, snapshot.MeetingDetected, snapshot.MeetingName,
                snapshot.Microphone,
                snapshot.Camera, snapshot.Hand, StatusMessageForSnapshot(snapshot), "current", force);
        }

        private async void SendTeamsCommandFromUi(TeamsCommand command)
        {
            var name = TeamsController.CommandName(command);
            var stopwatch = Stopwatch.StartNew();
            HostLog.Write(name + " requested.");
            if (IsTeamsDryRun)
            {
                TeamsText = "Dry run";
                HostLog.Write(name + " dry-run completed; Teams was not touched.");
                DetailText = "Dry run: " + name + " requested. Teams was not controlled.";
                QueueHostStatusIfChanged(true, false, string.Empty, CompanionTriState.Unknown, CompanionTriState.Unknown,
                    CompanionTriState.Unknown, "Dry-run " + name, "teams command dry-run");
                return;
            }

            var explicitPid = ParseCommandTargetProcessId();
            teamsCommandRefreshInFlight = true;
            try
            {
                var invoked = await Task.Run(() =>
                {
                    EnsureAudioProcessCacheForCommand(explicitPid);
                    return teamsController.TrySendCommand(command, mediaStatusSensor.TeamsAudioProcessIds, explicitPid);
                });
                if (isExiting || Dispatcher.HasShutdownStarted)
                {
                    return;
                }

                if (!invoked)
                {
                    HostLog.Write(name + " failed; Teams UIA control not found or not invokable.");
                    DetailText = "Teams control was not found. Start or join a meeting, then try again.";
                    var failedSnapshot = await Task.Run(() => ReadTeamsMeetingSnapshot(true, explicitPid));
                    if (isExiting || Dispatcher.HasShutdownStarted)
                    {
                        return;
                    }

                    QueueHostStatusIfChanged(failedSnapshot.TeamsDetected, failedSnapshot.MeetingDetected,
                        failedSnapshot.MeetingName, failedSnapshot.Microphone, failedSnapshot.Camera,
                        failedSnapshot.Hand, StatusMessageForSnapshot(failedSnapshot), "teams command missing");
                    return;
                }

                HostLog.Write(name + " invoked in Teams. elapsedMs=" + stopwatch.ElapsedMilliseconds);
                DetailText = name + " invoked in Teams.";
                var expectedState = GetExpectedStateForCommand(command);
                if (pendingButton.HasValue && PendingButtonMatchesCommand(command))
                {
                    pendingButtonExpectedState = expectedState;
                }

                HostLog.Write("Post-command waiting for observed state. command=" + name +
                    " expected=" + FormatTriState(expectedState) +
                    " elapsedMs=" + stopwatch.ElapsedMilliseconds);
                await RefreshAndSendCurrentStatusAfterInvokeAsync(command, name, expectedState, explicitPid,
                    stopwatch);
            }
            catch (System.Exception ex)
            {
                HostLog.Write("Teams command refresh failed.", ex);
            }
            finally
            {
                teamsCommandRefreshInFlight = false;
            }
        }

        private void EnsureAudioProcessCacheForCommand(int? explicitPid)
        {
            if (explicitPid.HasValue || mediaStatusSensor.TeamsAudioProcessIds.Count > 0)
            {
                return;
            }

            var teamsProcessIds = teamsController.TeamsProcessIds;
            if (teamsProcessIds.Count == 0)
            {
                return;
            }

            HostLog.Write("Teams command refreshing WASAPI audio PID cache before UIA target search.");
            mediaStatusSensor.GetMicrophoneState(teamsProcessIds);
        }

        private CompanionTriState? GetExpectedStateForCommand(TeamsCommand command)
        {
            if (lastSentHostState == null)
            {
                return null;
            }

            switch (command)
            {
                case TeamsCommand.ToggleMute:
                    return TryToggleTriState(lastSentHostState.Microphone, out var microphone) ? microphone : null;
                case TeamsCommand.ToggleVideo:
                    return TryToggleTriState(lastSentHostState.Camera, out var camera) ? camera : null;
                case TeamsCommand.ToggleHand:
                    return TryToggleTriState(lastSentHostState.Hand, out var hand) ? hand : null;
                default:
                    return null;
            }
        }

        private async Task RefreshAndSendCurrentStatusAfterInvokeAsync(TeamsCommand command, string commandName,
            CompanionTriState? expectedState, int? explicitTargetProcessId, Stopwatch commandStopwatch)
        {
            var delaysMs = new[] { 50, 100, 150, 250, 400, 650, 1000 };
            for (var attempt = 0; attempt < delaysMs.Length; attempt++)
            {
                await Task.Delay(delaysMs[attempt]);
                var snapshot = await Task.Run(() => ReadTeamsMeetingSnapshot(false, explicitTargetProcessId));
                if (isExiting || Dispatcher.HasShutdownStarted)
                {
                    return;
                }

                var observedState = GetCommandField(snapshot, command);
                var observedExpectedState = expectedState.HasValue &&
                    observedState.HasValue &&
                    observedState.Value == expectedState.Value;
                var finalAttempt = attempt == delaysMs.Length - 1;
                var canAcknowledgePendingButton = observedExpectedState && pendingButton.HasValue &&
                    PendingButtonMatchesCommand(command);
                var shouldSendObservedState = observedExpectedState || !pendingButton.HasValue || finalAttempt;
                HostLog.Write("Post-command Teams state refresh. command=" + commandName +
                    " attempt=" + (attempt + 1) + " delayMs=" + delaysMs[attempt] +
                    " expected=" + FormatTriState(expectedState) +
                    " observed=" + FormatTriState(observedState) +
                    " accepted=" + shouldSendObservedState +
                    " pendingAck=" + canAcknowledgePendingButton +
                    " elapsedMs=" + commandStopwatch.ElapsedMilliseconds);

                if (!shouldSendObservedState)
                {
                    continue;
                }

                await Dispatcher.InvokeAsync(new System.Action(() =>
                {
                    if (isExiting)
                    {
                        return;
                    }

                    ApplyTeamsSnapshotToUi(snapshot);
                    QueueHostStatusIfChanged(snapshot.TeamsDetected, snapshot.MeetingDetected, snapshot.MeetingName,
                        snapshot.Microphone, snapshot.Camera, snapshot.Hand, StatusMessageForSnapshot(snapshot),
                        "post-command refresh " + delaysMs[attempt] + "ms");
                }), DispatcherPriority.Send);
                return;
            }
        }

        private static bool TryToggleTriState(CompanionTriState current, out CompanionTriState next)
        {
            if (current == CompanionTriState.On)
            {
                next = CompanionTriState.Off;
                return true;
            }

            if (current == CompanionTriState.Off)
            {
                next = CompanionTriState.On;
                return true;
            }

            next = CompanionTriState.Unknown;
            return false;
        }

        private static CompanionTriState? GetCommandField(TeamsMeetingSnapshot snapshot, TeamsCommand command)
        {
            if (snapshot == null)
            {
                return null;
            }

            switch (command)
            {
                case TeamsCommand.ToggleMute:
                    return snapshot.Microphone;
                case TeamsCommand.ToggleVideo:
                    return snapshot.Camera;
                case TeamsCommand.ToggleHand:
                    return snapshot.Hand;
                default:
                    return null;
            }
        }

        private bool PendingButtonMatchesCommand(TeamsCommand command)
        {
            if (!pendingButton.HasValue)
            {
                return false;
            }

            return (pendingButton.Value == CompanionButton.ToggleMute && command == TeamsCommand.ToggleMute) ||
                (pendingButton.Value == CompanionButton.ToggleCamera && command == TeamsCommand.ToggleVideo) ||
                (pendingButton.Value == CompanionButton.ToggleHand && command == TeamsCommand.ToggleHand);
        }

        private static CompanionTriState StateForButton(CompanionButton button, CompanionTriState microphone,
            CompanionTriState camera, CompanionTriState hand)
        {
            if (button == CompanionButton.ToggleMute)
            {
                return microphone;
            }

            if (button == CompanionButton.ToggleCamera)
            {
                return camera;
            }

            return hand;
        }

        private static string FormatTriState(CompanionTriState? value)
        {
            return value.HasValue ? value.Value.ToString() : "n/a";
        }

        private TeamsMeetingSnapshot ReadTeamsMeetingSnapshot()
        {
            return ReadTeamsMeetingSnapshot(refreshAudioProcessCache: true);
        }

        private TeamsMeetingSnapshot ReadTeamsMeetingSnapshot(bool refreshAudioProcessCache)
        {
            return ReadTeamsMeetingSnapshot(refreshAudioProcessCache, ParseCommandTargetProcessId());
        }

        private TeamsMeetingSnapshot ReadTeamsMeetingSnapshot(bool refreshAudioProcessCache, int? explicitTargetProcessId)
        {
            var teamsProcessIds = teamsController.TeamsProcessIds;
            if (refreshAudioProcessCache && teamsProcessIds.Count > 0)
            {
                // WASAPI is no longer used as the mute source of truth, but it still gives us the Teams audio PID.
                // That PID usually points at a hosted process whose parent owns the meeting window we need to scan.
                mediaStatusSensor.GetMicrophoneState(teamsProcessIds);
            }

            return teamsController.GetMeetingSnapshot(mediaStatusSensor.TeamsAudioProcessIds,
                explicitTargetProcessId);
        }

        private static string TeamsTextForSnapshot(TeamsMeetingSnapshot snapshot)
        {
            if (!snapshot.TeamsDetected)
            {
                return "Not detected";
            }

            if (snapshot.MeetingDetected)
            {
                return string.IsNullOrWhiteSpace(snapshot.MeetingName)
                    ? "In meeting"
                    : "Meeting: " + snapshot.MeetingName;
            }

            return "Running";
        }

        private static string StatusMessageForSnapshot(TeamsMeetingSnapshot snapshot)
        {
            if (!snapshot.TeamsDetected)
            {
                return "Teams not found";
            }

            return snapshot.MeetingDetected ? "Teams meeting" : "Teams running";
        }

        private int? ParseCommandTargetProcessId()
        {
            if (string.IsNullOrWhiteSpace(CommandTargetProcessIdText))
            {
                return null;
            }

            if (int.TryParse(CommandTargetProcessIdText.Trim(), out var processId) && processId > 0)
            {
                HostLog.Write("Using explicit Teams command target PID. pid=" + processId);
                return processId;
            }

            HostLog.Write("Ignoring invalid Teams command target PID. value=\"" + CommandTargetProcessIdText + "\"");
            DetailText = "Command target PID is invalid. Enter a numeric PID or leave it blank.";
            return null;
        }

        private void QueueHostStatusIfChanged(bool teamsDetected, bool meetingDetected, string meetingName,
            CompanionTriState microphone, CompanionTriState camera, CompanionTriState hand, string message,
            string reason, bool force = false)
        {
            if (isExiting)
            {
                HostLog.Write("Host state write skipped while app is exiting. reason=" + reason);
                return;
            }

            if (!wasBleConnected)
            {
                HostLog.Write("Host state write skipped while BLE is disconnected. reason=" + reason);
                return;
            }

            var payload = BuildHostStatePayload(teamsDetected, meetingDetected, meetingName, microphone, camera, hand,
                message);
            if (!force && payload.SameAs(lastSentHostState))
            {
                HostLog.Write("Host state unchanged; BLE write skipped. reason=" + reason);
                return;
            }

            if (hostStateWriteInFlight)
            {
                pendingHostState = payload;
                pendingHostStateReason = reason;
                HostLog.Write("Host state write deferred while previous write is in flight. reason=" + reason);
                return;
            }

            _ = SendHostStatePayloadAsync(payload, reason);
        }

        private HostStatePayload BuildHostStatePayload(bool teamsDetected, bool meetingDetected, string meetingName,
            CompanionTriState microphone, CompanionTriState camera, CompanionTriState hand, string message)
        {
            if (pendingButton.HasValue && pendingButtonExpectedState.HasValue &&
                StateForButton(pendingButton.Value, microphone, camera, hand) == pendingButtonExpectedState.Value)
            {
                var counter = (ushort)(pendingButtonCounter & CompanionProtocol.StateCounterMask);
                if (pendingButton.Value == CompanionButton.ToggleMute)
                {
                    microphoneStateCounter = counter;
                }
                else if (pendingButton.Value == CompanionButton.ToggleHand)
                {
                    handStateCounter = counter;
                }
                else if (pendingButton.Value == CompanionButton.ToggleCamera)
                {
                    cameraStateCounter = counter;
                }
            }

            return new HostStatePayload(teamsDetected, meetingDetected, meetingName, microphone, camera, hand,
                message, 0, 0, microphoneStateCounter, cameraStateCounter,
                handStateCounter);
        }

        private async Task SendHostStatePayloadAsync(HostStatePayload payload, string reason)
        {
            hostStateWriteInFlight = true;
            try
            {
                HostLog.Write("Host state write queued. reason=" + reason);
                var sent = await connectionService.SendHostStatusAsync(payload.TeamsDetected, payload.MeetingDetected,
                    payload.MeetingName, payload.Microphone, payload.Camera, payload.Hand, payload.Message,
                    payload.TeamsCounter, payload.MeetingCounter, payload.MicrophoneCounter, payload.CameraCounter,
                    payload.HandCounter);
                if (sent)
                {
                    lastSentHostState = payload;
                    ClearPendingButtonIfAcknowledged(payload);
                }
            }
            finally
            {
                hostStateWriteInFlight = false;
                var pending = pendingHostState;
                var pendingReason = pendingHostStateReason;
                pendingHostState = null;
                pendingHostStateReason = null;
                if (!isExiting && pending != null && !pending.SameAs(lastSentHostState))
                {
                    _ = SendHostStatePayloadAsync(pending, pendingReason ?? "pending");
                }
            }
        }

        private void ClearPendingButtonIfAcknowledged(HostStatePayload payload)
        {
            if (!pendingButton.HasValue || payload.CounterFor(pendingButton.Value) != pendingButtonCounter)
            {
                return;
            }

            var clearedText = ButtonName(pendingButton.Value) + " #" + pendingButtonCounter + " acknowledged";
            pendingButton = null;
            pendingButtonCounter = 0;
            pendingButtonExpectedState = null;
            Dispatcher.BeginInvoke(new System.Action(() =>
            {
                if (!isExiting)
                {
                    ButtonProtocolText = "No pending button";
                    DetailText = clearedText;
                }
            }));
        }

        private void StopServicesForExit()
        {
            statusTimer.Stop();
            connectionService.StatusChanged -= OnConnectionStatusChanged;
            connectionService.ButtonEventReceived -= OnButtonEventReceived;
            connectionService.ParticipationEventReceived -= OnParticipationEventReceived;
            ResetHostStateWriteCache();
            wasBleConnected = false;
            connectionService.Dispose();
        }

        private void ResetHostStateWriteCache()
        {
            lastSentHostState = null;
            pendingHostState = null;
            pendingHostStateReason = null;
            hostStateWriteInFlight = false;
            pendingButton = null;
            pendingButtonCounter = 0;
            pendingButtonExpectedState = null;
            teamsCommandRefreshInFlight = false;
            ButtonProtocolText = "No pending button";
        }

        private void OnTestStatusChanged(string reason)
        {
            if (!IsTestMode)
            {
                return;
            }

            SendTestHostStatus(reason);
        }

        private void SendTestHostStatus(string reason, bool force = false)
        {
            ApplyTestStatusToUi();
            var message = string.IsNullOrWhiteSpace(TestMessage) ? "Test status" : TestMessage;
            DetailText = "Test mode sent " + reason + ": teams=" + TestTeamsToggleText +
                ", meeting=" + TestMeetingToggleText + ", mic=" + TestMicrophoneToggleText +
                ", camera=" + TestCameraToggleText + ", hand=" + TestHandToggleText + ".";
            QueueHostStatusIfChanged(TestTeamsDetected, TestMeetingDetected, TestMeetingName, testMicrophone,
                testCamera, testHand, message, "test " + reason, force);
        }

        private void ApplyTestStatusToUi()
        {
            TeamsText = TestTeamsDetected ? "Detected (test)" : "Not detected (test)";
            MeetingText = TestMeetingToggleText + " (test)";
            MicrophoneText = TestMicrophoneToggleText + " (test)";
            CameraText = TestCameraToggleText + " (test)";
            HandText = TestHandToggleText + " (test)";
        }

        private static CompanionTriState NextTriState(CompanionTriState value)
        {
            if (value == CompanionTriState.Unknown)
            {
                return CompanionTriState.Off;
            }

            return value == CompanionTriState.Off ? CompanionTriState.On : CompanionTriState.Unknown;
        }

        private static string TriStateText(CompanionTriState value, string offText, string onText)
        {
            if (value == CompanionTriState.Off)
            {
                return offText;
            }

            return value == CompanionTriState.On ? onText : "Unknown";
        }

        private static ushort NormalizeProtocolCounter(ushort value)
        {
            value = (ushort)(value & CompanionProtocol.StateCounterMask);
            return value == 0 ? (ushort)1 : value;
        }

        private static string ButtonName(CompanionButton button)
        {
            if (button == CompanionButton.ToggleMute)
            {
                return "Mute";
            }

            if (button == CompanionButton.ToggleHand)
            {
                return "Hand";
            }

            return "Camera";
        }

        private bool SetField(ref string field, string value, string propertyName)
        {
            if (field == value)
            {
                return false;
            }

            field = value;
            if (PropertyChanged != null)
            {
                PropertyChanged(this, new PropertyChangedEventArgs(propertyName));
            }

            return true;
        }

        private bool SetField(ref bool field, bool value, string propertyName)
        {
            if (field == value)
            {
                return false;
            }

            field = value;
            if (PropertyChanged != null)
            {
                PropertyChanged(this, new PropertyChangedEventArgs(propertyName));
            }

            return true;
        }
    }
}
