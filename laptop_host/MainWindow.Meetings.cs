using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace X3LaptopCompanion
{
    public partial class MainWindow
    {
        private readonly MeetingCalendarService meetingCalendar = new MeetingCalendarService();
        private readonly CancellationTokenSource meetingLifetime = new CancellationTokenSource();
        private IReadOnlyList<CalendarMeeting> calendarMeetings = Array.Empty<CalendarMeeting>();
        private DateTimeOffset nextCalendarRead = DateTimeOffset.MinValue;
        private bool meetingRefreshInFlight;
        private bool meetingJoinInFlight;
        private bool liveMeetingDetected;
        private string calendarError = string.Empty;
        private string meetingJoinMessage = string.Empty;
        private int meetingPageIndex;
        private ushort meetingRevision;
        private CompanionMeetingPage displayedMeetingPage;
        private string meetingPageKey;
        private string lastSentMeetingKey;

        private async Task RefreshMeetingListAsync()
        {
            if (isExiting || meetingRefreshInFlight) return;
            meetingRefreshInFlight = true;
            try
            {
                var now = DateTimeOffset.Now;
                if (!IsTestMode && !IsTeamsDryRun && !liveMeetingDetected && !meetingJoinInFlight &&
                    now >= nextCalendarRead)
                {
                    nextCalendarRead = now.AddSeconds(30);
                    try
                    {
                        calendarMeetings = await meetingCalendar.ReadAsync(now, meetingLifetime.Token);
                        calendarError = string.Empty;
                    }
                    catch (OperationCanceledException) when (meetingLifetime.IsCancellationRequested)
                    {
                        return;
                    }
                    catch (Exception ex)
                    {
                        calendarMeetings = Array.Empty<CalendarMeeting>();
                        calendarError = ex.Message;
                        DetailText = calendarError;
                        HostLog.Write("Calendar refresh failed.", ex);
                    }
                }
                if (isExiting) return;
                var nearby = IsTestMode || IsTeamsDryRun
                    ? Array.Empty<CalendarMeeting>()
                    : MeetingCalendarService.Nearby(calendarMeetings, DateTimeOffset.Now);
                var pageCount = Math.Max(1, (nearby.Count + CompanionProtocol.MeetingsPerPage - 1) /
                    CompanionProtocol.MeetingsPerPage);
                meetingPageIndex = Math.Clamp(meetingPageIndex, 0, pageCount - 1);
                var rows = nearby.Skip(meetingPageIndex * CompanionProtocol.MeetingsPerPage)
                    .Take(CompanionProtocol.MeetingsPerPage).ToArray();
                byte status = meetingJoinInFlight ? (byte)1 : string.IsNullOrEmpty(calendarError) ? (byte)0 : (byte)2;
                var message = IsTestMode || IsTeamsDryRun ? "Calendar disabled in test / dry-run mode" :
                    meetingJoinInFlight ? "Joining muted, camera off..." :
                    !string.IsNullOrEmpty(calendarError) ? calendarError :
                    !string.IsNullOrEmpty(meetingJoinMessage) ? meetingJoinMessage :
                    rows.Length == 0 ? "No meetings in the -15 / +40 minute window" : "Tap to join muted, camera off";
                var key = JsonSerializer.Serialize(new { meetingPageIndex, pageCount, status, message, rows });
                if (displayedMeetingPage == null || key != meetingPageKey)
                {
                    meetingRevision = meetingRevision == ushort.MaxValue ? (ushort)1 : (ushort)(meetingRevision + 1);
                    displayedMeetingPage = new CompanionMeetingPage(meetingRevision, checked((ushort)meetingPageIndex),
                        checked((ushort)pageCount), status, message, rows);
                    meetingPageKey = key;
                }
                if (wasBleConnected && key != lastSentMeetingKey &&
                    await connectionService.SendMeetingListAsync(displayedMeetingPage))
                    lastSentMeetingKey = key;
            }
            catch (OperationCanceledException) when (meetingLifetime.IsCancellationRequested)
            {
                // Shutdown cancels the calendar sidecar and the join wait.
            }
            catch (Exception ex)
            {
                DetailText = "Meeting list failed: " + ex.Message;
                HostLog.Write("Meeting list refresh failed.", ex);
            }
            finally
            {
                meetingRefreshInFlight = false;
            }
        }

        private async Task HandleMeetingRequestAsync(CompanionButtonEvent request)
        {
            if (isExiting) return;
            var startedJoin = false;
            try
            {
                if (meetingJoinInFlight)
                {
                    SetButtonProtocolText("Meeting join already in progress");
                    return;
                }
                if (IsTestMode || IsTeamsDryRun || liveMeetingDetected)
                    throw new InvalidOperationException("Meeting join is available only when not in a meeting, in live mode.");
                if (displayedMeetingPage == null || request.MeetingRevision != displayedMeetingPage.Revision)
                    throw new InvalidOperationException("Meeting list changed; select the meeting again.");
                if (request.Button != CompanionButton.JoinMeeting)
                {
                    meetingPageIndex += request.Button == CompanionButton.NextMeetings ? 1 : -1;
                    meetingJoinMessage = string.Empty;
                    await RefreshMeetingListAsync();
                    return;
                }
                var meeting = displayedMeetingPage.ResolveJoin(request.Argument, request.MeetingRevision, DateTimeOffset.Now);
                meetingJoinInFlight = true;
                startedJoin = true;
                meetingJoinMessage = string.Empty;
                await RefreshMeetingListAsync();
                var result = await Task.Run(() => teamsController.JoinMeetingMutedAsync(meeting, meetingLifetime.Token));
                if (!isExiting)
                {
                    meetingJoinMessage = result;
                    DetailText = result;
                    HostLog.Write("Meeting join completed muted with camera off.");
                }
            }
            catch (OperationCanceledException) when (meetingLifetime.IsCancellationRequested)
            {
                return;
            }
            catch (Exception ex)
            {
                meetingJoinMessage = ex.Message;
                DetailText = "Meeting join: " + ex.Message;
                HostLog.Write("Meeting request failed.", ex);
            }
            finally
            {
                if (startedJoin) meetingJoinInFlight = false;
            }
            await RefreshMeetingListAsync();
            if (!isExiting && !IsTestMode && !IsTeamsDryRun && !statusRefreshInFlight)
            {
                statusRefreshInFlight = true;
                await RefreshLiveTeamsStatusAsync(ParseCommandTargetProcessId(), force: true);
            }
        }
    }
}
