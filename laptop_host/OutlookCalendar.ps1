param(
    [Parameter(Mandatory = $true)][string]$FromIso,
    [Parameter(Mandatory = $true)][string]$ToIso
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$objects = New-Object System.Collections.Generic.List[object]

function Keep-ComObject($value) {
    if ($null -eq $value) { throw "Outlook returned an empty calendar object." }
    $objects.Add($value)
    return ,$value
}

try {
    $from = [DateTimeOffset]::Parse($FromIso).LocalDateTime
    $to = [DateTimeOffset]::Parse($ToIso).LocalDateTime
    $outlook = Keep-ComObject (New-Object -ComObject Outlook.Application)
    $namespace = Keep-ComObject ($outlook.GetNamespace("MAPI"))
    $calendar = Keep-ComObject ($namespace.GetDefaultFolder(9))
    $items = Keep-ComObject ($calendar.Items)
    $items.Sort("[Start]")
    $items.IncludeRecurrences = $true
    # Restrict's local date format omits seconds; widen it before applying the exact bounds below.
    $restriction = "[Start] <= '$($to.AddMinutes(1).ToString("g"))' AND [End] >= '$($from.AddMinutes(-1).ToString("g"))'"
    $calendarMatches = Keep-ComObject ($items.Restrict($restriction))
    $meetings = @{}
    foreach ($item in $calendarMatches) {
        try {
            $start = [DateTime]$item.Start
            $end = [DateTime]$item.End
            if ($end -lt $from -or $start -gt $to) { continue }
            # Cancelled appointments are not joinable. Do not filter ResponseStatus:
            # tentative invitations and invitations not yet accepted belong in the list.
            if ([int]$item.MeetingStatus -in @(5, 7)) { continue }
            $content = [string]$item.Location + "`n" + [string]$item.Body
            if ($content -notmatch 'https://teams\.microsoft\.com/(?:l/(?:meetup-join|meeting)|meet)/[^\s"''<>]+') {
                continue
            }
            $joinUrl = [Net.WebUtility]::HtmlDecode($Matches[0]).TrimEnd(".", ",", ")", "]")
            $sourceId = [string]$item.GlobalAppointmentID
            if (-not $sourceId) { $sourceId = [string]$item.EntryID }
            $id = $sourceId + "|" + $start.ToString("o")
            $meetings[$id] = @{
                id = $id
                subject = (([string]$item.Subject -replace '[\x00-\x1F]', ' ') -replace '\s+', ' ').Trim()
                start = ([DateTimeOffset]$start).ToString("o")
                end = ([DateTimeOffset]$end).ToString("o")
                joinUrl = $joinUrl
            }
        } catch {
            [Console]::Error.WriteLine("An Outlook calendar item could not be read: " + $_.Exception.Message)
        } finally {
            if ($null -ne $item -and [Runtime.InteropServices.Marshal]::IsComObject($item)) {
                [void][Runtime.InteropServices.Marshal]::ReleaseComObject($item)
            }
        }
    }
    @{ ok = $true; meetings = @($meetings.Values) } | ConvertTo-Json -Compress -Depth 4
} catch {
    @{ ok = $false; detail = "Outlook calendar unavailable: $($_.Exception.Message)"; meetings = @() } |
        ConvertTo-Json -Compress -Depth 4
} finally {
    for ($i = $objects.Count - 1; $i -ge 0; $i--) {
        if ([Runtime.InteropServices.Marshal]::IsComObject($objects[$i])) {
            [void][Runtime.InteropServices.Marshal]::ReleaseComObject($objects[$i])
        }
    }
}
