using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Windows.Automation;
using X3LaptopCompanion;

internal static partial class Program
{
    private static void CheckMeetingFeatures()
    {
        var now = new DateTimeOffset(2026, 10, 20, 20, 0, 0, TimeSpan.FromHours(-7));
        const string url = "https://teams.microsoft.com/l/meetup-join/19%3ameeting_test/0?context=%7b%7d";
        CalendarMeeting Meeting(string id, double start, double end) =>
            new CalendarMeeting(id, id, now.AddMinutes(start), now.AddMinutes(end), url);
        var eligible = new[]
        {
            Meeting("ongoing-long", -120, 1),
            Meeting("ended-boundary", -60, -15),
            Meeting("future-boundary", 40, 80),
            Meeting("unaccepted", 5, 35),
            Meeting("cross-offset", 10, 20) with
            {
                Start = now.AddMinutes(10).ToOffset(TimeSpan.FromHours(5.5)),
                End = now.AddMinutes(20).ToOffset(TimeSpan.FromHours(5.5))
            }
        };
        foreach (var meeting in eligible) Assert(meeting.IsNearby(now), "Eligible meeting excluded: " + meeting.Id);
        Assert(!Meeting("too-old", -60, -15.001).IsNearby(now), "End cutoff must be exact.");
        Assert(!Meeting("too-future", 40.001, 50).IsNearby(now), "Start cutoff must be exact.");
        Assert(!Meeting("invalid", 20, 10).IsNearby(now), "Negative duration must be rejected.");
        var nearby = MeetingCalendarService.Nearby(eligible.Concat(new[]
        {
            eligible[0], Meeting("too-old", -80, -16), Meeting("too-future", 41, 60),
            Meeting("not-teams", 0, 1) with { JoinUrl = "https://example.com/meeting" }
        }), now);
        Assert(nearby.Count == eligible.Length, "Nearby list must deduplicate and filter without acceptance checks.");
        Assert(nearby.Select(m => m.Start).SequenceEqual(nearby.Select(m => m.Start).OrderBy(t => t)),
            "Meeting list must be sorted by start instant.");

        foreach (var value in new[] { url, "https://teams.microsoft.com/meet/123?p=secret",
            "https://teams.microsoft.com/l/meeting/123", "https://TEAMS.MICROSOFT.COM/meet/123" })
            Assert(MeetingCalendarService.IsTeamsJoinUrl(value), "Valid Teams URL rejected.");
        foreach (var value in new[] { null, "", "file:///C:/secret", "msteams://teams.microsoft.com/meet/123",
            "http://teams.microsoft.com/meet/123", "https://teams.microsoft.com.evil.test/meet/123",
            "https://teams.microsoft.com@evil.test/meet/123", "https://evil@teams.microsoft.com/meet/123",
            "https://teams.microsoft.com:444/meet/123", "https://teams.microsoft.com/meet/",
            "https://teams.microsoft.com/chat/123" })
            Assert(!MeetingCalendarService.IsTeamsJoinUrl(value), "Unsafe join URL accepted: " + value);

        var json = JsonSerializer.Serialize(new
        {
            ok = true,
            meetings = eligible.Select(m => new { id = m.Id, subject = m.Subject,
                start = m.Start, end = m.End, joinUrl = m.JoinUrl, responseStatus = "notResponded" })
        });
        Assert(MeetingCalendarService.Parse(json, now).Count == eligible.Length,
            "Unaccepted calendar invitations must be included.");
        Assert(MeetingCalendarService.Parse("""{"ok":true,"meetings":[]}""", now).Count == 0,
            "Empty calendar is valid.");
        AssertThrows<InvalidOperationException>(() =>
            MeetingCalendarService.Parse("""{"ok":false,"detail":"Outlook unavailable","meetings":[]}""", now));
        AssertThrows<JsonException>(() => MeetingCalendarService.Parse("not json", now));

        var page = new CompanionMeetingPage(513, 1, 2, 0, "Tap to join",
            eligible.Take(4).ToArray());
        Assert(page.ResolveJoin(3, 513, now).Id == "unaccepted", "Join must resolve the displayed slot.");
        AssertThrows<InvalidOperationException>(() => page.ResolveJoin(0, 512, now));
        AssertThrows<InvalidOperationException>(() => page.ResolveJoin(4, 513, now));
        AssertThrows<InvalidOperationException>(() => (page with { Status = 1 }).ResolveJoin(0, 513, now));
        AssertThrows<InvalidOperationException>(() => (page with { Status = 2 }).ResolveJoin(0, 513, now));
        AssertThrows<InvalidOperationException>(() => page.ResolveJoin(1, 513, now.AddTicks(1)));

        var payload = CompanionProtocol.EncodeMeetingPage(page);
        using (var reader = new BinaryReader(new MemoryStream(payload), Encoding.UTF8))
        {
            Assert(reader.ReadUInt16() == 513, "Meeting revision must be little-endian.");
            Assert(reader.ReadUInt16() == 1 && reader.ReadUInt16() == 2, "Meeting page indices mismatch.");
            Assert(reader.ReadByte() == 0 && reader.ReadByte() == 4, "Meeting status and count mismatch.");
            Assert(Encoding.UTF8.GetString(reader.ReadBytes(reader.ReadByte())) == page.Message, "Meeting message mismatch.");
            foreach (var meeting in page.Meetings)
            {
                Assert(reader.ReadUInt16() == meeting.Start.LocalDateTime.Hour * 60 + meeting.Start.LocalDateTime.Minute,
                    "Start time encoding mismatch.");
                Assert(reader.ReadUInt16() == meeting.End.LocalDateTime.Hour * 60 + meeting.End.LocalDateTime.Minute,
                    "End time encoding mismatch.");
                Assert(Encoding.UTF8.GetString(reader.ReadBytes(reader.ReadByte())) == meeting.Subject,
                    "Title encoding mismatch.");
            }
            Assert(reader.BaseStream.Position == reader.BaseStream.Length, "Unexpected meeting payload bytes.");
        }
        var unicodeMeeting = eligible[0] with { Subject = new string('a', 63) + "\u00e9 tail" };
        var unicodePayload = CompanionProtocol.EncodeMeetingPage(new CompanionMeetingPage(1, 0, 1, 0,
            new string('x', 79) + "\u00e9", new[] { unicodeMeeting }));
        Assert(unicodePayload[8] == 79 && unicodePayload[9 + 79 + 4] == 63,
            "Meeting text must truncate without splitting UTF-8 characters.");
        var maximum = CompanionProtocol.EncodeMeetingPage(page with
        {
            Message = new string('m', 80),
            Meetings = Enumerable.Repeat(unicodeMeeting with { Subject = new string('n', 64) }, 4).ToArray()
        });
        Assert(maximum.Length == 365, "Maximum meeting payload must match firmware bounds.");
        foreach (var size in new[] { 20, 23, 64, 180, 512 })
        {
            var fragments = CompanionProtocol.MeetingFragments(maximum, size).ToArray();
            using var combined = new MemoryStream();
            foreach (var fragment in fragments)
            {
                Assert(fragment.Length <= size && fragment.Length > 5, "Fragment exceeds ATT payload.");
                Assert(fragment[0] == CompanionProtocol.ProtocolVersion, "Fragment protocol version mismatch.");
                Assert((fragment[1] | fragment[2] << 8) == combined.Length, "Fragment offset mismatch.");
                Assert((fragment[3] | fragment[4] << 8) == maximum.Length, "Fragment total mismatch.");
                combined.Write(fragment, 5, fragment.Length - 5);
            }
            Assert(combined.ToArray().SequenceEqual(maximum), "Fragmented page does not round-trip.");
        }
        AssertThrows<ArgumentException>(() => CompanionProtocol.MeetingFragments(payload, 5).ToArray());
        AssertThrows<ArgumentException>(() => CompanionProtocol.EncodeMeetingPage(page with { Revision = 0 }));
        AssertThrows<ArgumentException>(() => CompanionProtocol.EncodeMeetingPage(page with { PageCount = 0 }));
        AssertThrows<ArgumentException>(() => CompanionProtocol.EncodeMeetingPage(page with { PageIndex = 2 }));
        AssertThrows<ArgumentException>(() => CompanionProtocol.EncodeMeetingPage(page with { Meetings = eligible }));
        var desktops = new CompanionDesktopState(new[] { new CompanionDesktopInfo("A", false) }, 0, 257);
        Assert(CompanionProtocol.EncodeDesktopState(desktops, 20).SequenceEqual(
            new byte[] { CompanionProtocol.ProtocolVersion, 1, 0, 1, 1, 0, 1, (byte)'A' }),
            "The existing desktop payload shape must be preserved.");
        using (var cancelled = new System.Threading.CancellationTokenSource())
        {
            cancelled.Cancel();
            AssertThrows<OperationCanceledException>(() => new MeetingCalendarService().ReadAsync(now, cancelled.Token)
                .GetAwaiter().GetResult());
            AssertThrows<OperationCanceledException>(() => new TeamsController().JoinMeetingMutedAsync(eligible[0],
                cancelled.Token).GetAwaiter().GetResult());
        }

        var mediaCases = new (string Name, bool Camera, CompanionTriState State)[]
        {
            ("Camera off", true, CompanionTriState.Off),
            ("Camera is on", true, CompanionTriState.On),
            ("Turn camera on", true, CompanionTriState.Off),
            ("Turn the camera off (Ctrl+Shift+O)", true, CompanionTriState.On),
            ("Video off", true, CompanionTriState.Off),
            ("Unmute mic", false, CompanionTriState.Off),
            ("Mute microphone", false, CompanionTriState.On),
            ("Microphone muted", false, CompanionTriState.Off),
            ("Mic on", false, CompanionTriState.On),
            ("Unmute", false, CompanionTriState.Off),
            ("Mute mic (Ctrl+Shift+M)", false, CompanionTriState.On),
            ("Mute speaker", false, CompanionTriState.Unknown),
            ("Unmute mic", true, CompanionTriState.Unknown),
            ("Turn camera off", false, CompanionTriState.Unknown),
            ("Camera", true, CompanionTriState.Unknown),
            ("Microphone", false, CompanionTriState.Unknown),
            (null, true, CompanionTriState.Unknown)
        };
        foreach (var test in mediaCases)
            Assert(TeamsController.ParsePreJoinMediaState(test.Name, test.Camera) == test.State,
                "Incorrect prejoin state for " + test.Name);
        Assert(TeamsController.ResolvePreJoinMediaState(CompanionTriState.Unknown, null) == CompanionTriState.Unknown,
            "Missing state must never count as off.");
        Assert(TeamsController.ResolvePreJoinMediaState(CompanionTriState.Unknown, ToggleState.Off) == CompanionTriState.Off,
            "Generic switch state should be read from TogglePattern.");
        Assert(TeamsController.ResolvePreJoinMediaState(CompanionTriState.Off, ToggleState.On) == CompanionTriState.Unknown,
            "Conflicting named/toggle state must prevent joining.");
        Assert(TeamsController.ResolvePreJoinMediaState(CompanionTriState.Off, ToggleState.Indeterminate) ==
            CompanionTriState.Unknown, "Indeterminate media state must prevent joining.");
    }
}
