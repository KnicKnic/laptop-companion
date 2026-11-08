using System;
using System.Collections.Generic;

namespace X3LaptopCompanion
{
    public sealed record CompanionMeetingPage(ushort Revision, ushort PageIndex, ushort PageCount,
        byte Status, string Message, IReadOnlyList<CalendarMeeting> Meetings)
    {
        internal CalendarMeeting ResolveJoin(byte index, ushort revision, DateTimeOffset now)
        {
            if (revision != Revision || index >= Meetings.Count || Status != 0)
                throw new InvalidOperationException("Meeting list changed; select the meeting again.");
            var meeting = Meetings[index];
            if (!meeting.IsNearby(now))
                throw new InvalidOperationException("Meeting is outside the -15 / +40 minute window.");
            return meeting;
        }
    }
}
