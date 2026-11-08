$ErrorActionPreference = "Stop"
$now = [DateTimeOffset]::Parse("2026-10-20T20:00:30-07:00")
$script:from = $now.AddMinutes(-15).LocalDateTime
$script:to = $now.AddMinutes(40).LocalDateTime
$url = "https://teams.microsoft.com/l/meetup-join/test/0?context=one&amp;two=three"

function Appointment([string]$id, [DateTime]$start, [DateTime]$end, [int]$status = 3, [string]$body = $url) {
    return [pscustomobject]@{
        GlobalAppointmentID = $id
        EntryID = "entry-$id"
        Start = $start
        End = $end
        Subject = "Meeting`n$id"
        Location = ""
        Body = $body
        MeetingStatus = $status
        ResponseStatus = 5
    }
}

$script:appointments = @(
    (Appointment "ongoing" ($script:from.AddHours(-2)) ($script:to))
    (Appointment "end-boundary" ($script:from.AddHours(-1)) $script:from)
    (Appointment "future-boundary" $script:to ($script:to.AddHours(1)))
    (Appointment "unaccepted" ($script:from.AddMinutes(20)) ($script:to))
    (Appointment "too-old" ($script:from.AddHours(-1)) ($script:from.AddTicks(-1)))
    (Appointment "too-future" ($script:to.AddTicks(1)) ($script:to.AddHours(1)))
    (Appointment "cancelled" $script:from $script:to 5)
    (Appointment "received-cancelled" $script:from $script:to 7)
    (Appointment "no-link" $script:from $script:to 3 "An ordinary calendar appointment")
    (Appointment "duplicate" $script:from $script:to)
    (Appointment "duplicate" $script:from $script:to)
    (Appointment "" ($script:from.AddMinutes(1)) $script:to)
    (Appointment "recurring" $script:from $script:to)
    (Appointment "recurring" ($script:from.AddMinutes(2)) $script:to)
)
$items = [pscustomobject]@{
    IncludeRecurrences = $false
    Sorted = $false
    Appointments = $script:appointments
    Restriction = "[Start] <= '$($script:to.AddMinutes(1).ToString("g"))' AND [End] >= '$($script:from.AddMinutes(-1).ToString("g"))'"
}
$items | Add-Member ScriptMethod Sort {
    param($field)
    if ($field -ne "[Start]") { throw "Calendar must sort by Start." }
    $this.Sorted = $true
}
$items | Add-Member ScriptMethod Restrict {
    param($restriction)
    if (-not $this.Sorted -or -not $this.IncludeRecurrences) {
        throw "Calendar must sort and expand recurring occurrences before restricting."
    }
    if ($restriction -ne $this.Restriction) { throw "Incorrect calendar query boundaries." }
    return $this.Appointments
}
$calendar = [pscustomobject]@{ Items = $items }
$namespace = [pscustomobject]@{ Calendar = $calendar }
$namespace | Add-Member ScriptMethod GetDefaultFolder {
    param($index)
    if ($index -ne 9) { throw "Expected default Outlook calendar." }
    return $this.Calendar
}
$script:outlook = [pscustomobject]@{ Namespace = $namespace }
$script:outlook | Add-Member ScriptMethod GetNamespace {
    param($name)
    if ($name -ne "MAPI") { throw "Expected MAPI namespace." }
    return $this.Namespace
}

$fakeState = [pscustomobject]@{ Application = $script:outlook }
Set-Item Function:\New-Object {
    [CmdletBinding(DefaultParameterSetName = "type")]
    param(
        [Parameter(Position = 0, ParameterSetName = "type")][string]$TypeName,
        [Parameter(ParameterSetName = "com")][string]$ComObject,
        [Parameter(Position = 1, ParameterSetName = "type")][object[]]$ArgumentList
    )
    if ($ComObject -eq "Outlook.Application") { return $fakeState.Application }
    return Microsoft.PowerShell.Utility\New-Object @PSBoundParameters
}.GetNewClosure()

$reader = Join-Path $PSScriptRoot "..\laptop_host\OutlookCalendar.ps1"
$result = (& $reader -FromIso $now.AddMinutes(-15).ToString("o") -ToIso $now.AddMinutes(40).ToString("o")) |
    ConvertFrom-Json
if (-not $result.ok) { throw $result.detail }
if ($result.meetings.Count -ne 8) { throw "Expected eight unique eligible meeting occurrences." }
foreach ($id in @("ongoing", "end-boundary", "future-boundary", "unaccepted", "duplicate")) {
    if (-not ($result.meetings | Where-Object { $_.id.StartsWith("$id|") })) { throw "Missing eligible meeting $id." }
}
if (@($result.meetings | Where-Object { $_.id.StartsWith("recurring|") }).Count -ne 2) {
    throw "Separate recurring occurrences must be preserved."
}
if (-not ($result.meetings | Where-Object { $_.id.StartsWith("entry-|") })) {
    throw "EntryID must be used if GlobalAppointmentID is empty."
}
if ($result.meetings | Where-Object { $_.subject.Contains("`n") -or $_.joinUrl.Contains("&amp;") }) {
    throw "Calendar titles and join links must be normalized."
}
$fakeState.Application = $null
$failure = (& $reader -FromIso $now.AddMinutes(-15).ToString("o") -ToIso $now.AddMinutes(40).ToString("o")) |
    ConvertFrom-Json
if ($failure.ok -or -not $failure.detail -or $failure.meetings.Count -ne 0) {
    throw "Calendar errors must be explicit and must not return stale meetings."
}
Write-Output "Passed Outlook calendar fixture checks. No Outlook objects were created."
