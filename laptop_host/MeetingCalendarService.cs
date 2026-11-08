using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace X3LaptopCompanion
{
    public sealed record CalendarMeeting(string Id, string Subject, DateTimeOffset Start,
        DateTimeOffset End, string JoinUrl)
    {
        public bool IsNearby(DateTimeOffset now) =>
            End >= Start && End >= now.AddMinutes(-15) && Start <= now.AddMinutes(40);
    }

    public sealed class MeetingCalendarService
    {
        internal static bool IsTeamsJoinUrl(string value) =>
            Uri.TryCreate(value, UriKind.Absolute, out var uri) &&
            uri.Scheme == Uri.UriSchemeHttps &&
            uri.Host.Equals("teams.microsoft.com", StringComparison.OrdinalIgnoreCase) &&
            uri.IsDefaultPort && string.IsNullOrEmpty(uri.UserInfo) &&
            Regex.IsMatch(uri.AbsolutePath, @"^/(?:l/(?:meetup-join|meeting)|meet)/[^/]+",
                RegexOptions.CultureInvariant);

        internal static IReadOnlyList<CalendarMeeting> Nearby(IEnumerable<CalendarMeeting> meetings,
            DateTimeOffset now) => meetings.Where(m => m.IsNearby(now) && IsTeamsJoinUrl(m.JoinUrl))
                .DistinctBy(m => m.Id).OrderBy(m => m.Start).ThenBy(m => m.Id, StringComparer.Ordinal).ToArray();

        public async Task<IReadOnlyList<CalendarMeeting>> ReadAsync(DateTimeOffset now, CancellationToken token)
        {
            token.ThrowIfCancellationRequested();
            var startInfo = new ProcessStartInfo("powershell.exe")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8
            };
            foreach (var arg in new[] { "-NoProfile", "-NonInteractive", "-STA", "-File",
                Path.Combine(AppContext.BaseDirectory, "OutlookCalendar.ps1"), "-FromIso",
                now.AddMinutes(-15).ToString("o"), "-ToIso", now.AddMinutes(40).ToString("o") })
            {
                startInfo.ArgumentList.Add(arg);
            }

            using var process = Process.Start(startInfo) ??
                throw new InvalidOperationException("Outlook calendar reader could not be started.");
            var output = process.StandardOutput.ReadToEndAsync(token);
            var errors = process.StandardError.ReadToEndAsync(token);
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
            timeout.CancelAfter(TimeSpan.FromSeconds(60));
            try
            {
                await process.WaitForExitAsync(timeout.Token);
            }
            catch (OperationCanceledException)
            {
                if (!process.HasExited) process.Kill(entireProcessTree: true);
                await process.WaitForExitAsync(CancellationToken.None);
                token.ThrowIfCancellationRequested();
                throw new TimeoutException("Outlook calendar read timed out.");
            }

            var error = await errors;
            if (!string.IsNullOrWhiteSpace(error)) HostLog.Write("Outlook calendar reader: " + error.Trim());
            if (process.ExitCode != 0)
                throw new InvalidOperationException("Outlook calendar reader failed: " + error.Trim());
            return Parse(await output, now);
        }

        internal static IReadOnlyList<CalendarMeeting> Parse(string json, DateTimeOffset now)
        {
            using var document = JsonDocument.Parse(json);
            var root = document.RootElement;
            if (!root.GetProperty("ok").GetBoolean())
                throw new InvalidOperationException(root.GetProperty("detail").GetString());
            var meetings = new List<CalendarMeeting>();
            foreach (var item in root.GetProperty("meetings").EnumerateArray())
            {
                meetings.Add(new CalendarMeeting(item.GetProperty("id").GetString(),
                    item.GetProperty("subject").GetString() ?? "Meeting",
                    DateTimeOffset.Parse(item.GetProperty("start").GetString(), CultureInfo.InvariantCulture),
                    DateTimeOffset.Parse(item.GetProperty("end").GetString(), CultureInfo.InvariantCulture),
                    item.GetProperty("joinUrl").GetString()));
            }
            return Nearby(meetings, now);
        }
    }
}
