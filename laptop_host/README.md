# X3 Laptop Companion Host

.NET 10 WPF tray app for the X3 BLE Laptop Companion.

## Requirements

- Windows 10 19041 or newer.
- .NET 10 SDK.
- Bluetooth LE adapter.
- Classic Outlook desktop signed into the default calendar, for nearby meeting
  enumeration. New Outlook alone does not expose the local Outlook object model.

## Current Status

- Scans for the X3 companion GATT service.
- Subscribes to device-command notifications.
- Sends Teams presence, meeting presence, meeting name, microphone, camera, and
  hand state to the X3.
- Sends the list of Windows virtual desktops and the active one to the X3, and
  handles the X3's desktop switch requests.
  - Desktops are enumerated through the shell's internal virtual desktop manager
    so that a remote machine added to Task view by the Windows App (Windows 365
    Switch / Cloud PC) is included; the registry desktop list is the fallback.
  - A desktop the shell reports but the registry does not list is a remote
    desktop; it is flagged for the X3 and named after its remote session window.
  - A remote machine that is registered with Task view but has no live session
    (read from `Explorer\RemoteSystemProviders`) is shown as a disconnected
    desktop, but it cannot be connected from the X3: selecting it just reports
    that it is offline. Connecting is left to Task view because the shell call
    that attaches a remote desktop (`CreateRemoteDesktop`) adds a new Task view
    tile rather than reusing the registered machine, which corrupts the
    Windows 365 Switch state. Once connected from Task view the tile becomes a
    normal switchable desktop here.
  - Switching asks the shell to activate the target desktop directly, which also
    works for the remote desktop and avoids stepping through the desktops in
    between. If that call is unavailable on the running Windows build, it falls
    back to stepping with `Ctrl+Win+Left/Right`.
- Finds the active Teams meeting window from the parent of the Teams audio
  process when possible, preferring the full meeting toolbar with its hand button
  over compact/sharing windows that only expose microphone and camera controls.
- Handles mute, camera, and raise-hand commands by finding the current Teams UI
  Automation button and invoking it with `InvokePattern`. The meeting window is
  cached, but controls are rediscovered for each command because Teams rerenders
  the meeting toolbar.
- Finds the hand button by its automation id (`raisehands-button`) and reads its
  native UI Automation `FullDescription` (property 30159), not its fixed `Raise`
  label. `Lower your hand` means raised; `Raise your hand` means lowered. The
  description is readable without hovering or focusing Teams, including suffixes
  such as `. Has context menu`. The host does not guess or toggle a local hand
  state, and post-command refreshes wait for the observed state.
  Missing or unrecognized descriptions are logged and reported as unknown, with
  the hand tile locked rather than assuming the hand is lowered. As before, an
  unreachable meeting window holds the last observed state with the tile locked.
  Hand-state descriptions currently require English Teams text.
- Handles meeting reactions (like, heart, applause) by expanding the meeting's
  React menu button and then invoking the reaction inside the flyout, searching
  the desktop root as well because the flyout is a popup window. The React
  control only supports `ExpandCollapsePattern`, not `InvokePattern`. Controls
  are matched on their Teams automation ids (`reaction-menu-button`,
  `like-button`, `heart-button`, `applause-button`) so the lookup survives UI
  language changes, with display names as a fallback. When nothing matches,
  every visible button name is written to the log.
- Speaker toggle is not sent until a matching Teams UI Automation button name is
  identified; keyboard hotkeys are not used.
- The GUI can dump a selected window's UI Automation tree to the host log.
  The window target accepts `hwnd:0x...`, `0x...`, `pid:1234`, or a title
  fragment; use `List` to discover visible top-level windows. Dumps focus on
  the selected process's RawView subtree and log upward paths for buttons named
  `Unmute`.
- Includes a test mode that keeps BLE active and sends simulated Teams,
  meeting, microphone, camera, hand, meeting-name, and status-message values to
  the X3.
- Includes a Teams dry-run mode that keeps BLE active and acknowledges X3 mute
  commands without focusing or controlling Teams.
- When not in a meeting, reads the default Outlook calendar every 30 seconds
  through the bundled `OutlookCalendar.ps1` sidecar. It expands recurring
  occurrences, includes invitations regardless of acceptance status, and excludes
  cancelled meetings and entries without a Teams join link. The time filter is
  inclusive: `End >= now - 15 minutes` and `Start <= now + 40 minutes`, so an
  ongoing meeting is included even if it began more than 15 minutes ago.
- Sends four meetings per device page, with previous/next navigation for every
  remaining match. Tap a row to join, or use Up/Down to select, Confirm to join,
  and Left/Right to change pages. Times use the laptop's local clock.
  The idle screen hides Bluetooth, microphone, camera, hand and reaction icons,
  while retaining the desktop switcher.
- Opens a selected meeting in desktop Teams, sets its pre-join camera off and
  microphone muted (without disabling incoming audio), re-reads both states, then
  invokes Join now. Missing, ambiguous or conflicting media controls stop the join
  instead of assuming they are off. Pre-join state labels currently require
  English Teams text. Existing calls/pre-join screens must be closed first.
  Joining does not start recording or transcription.
- Reports calendar/join failures on the device and in the host log. Calendar
  access and joining are disabled in test/dry-run modes. Calendar reads and join
  waits are cancelled on host shutdown.
- Writes a diagnostic log to `%LOCALAPPDATA%\X3LaptopCompanion\host.log`.
  Use the `Open Log` button or tray menu item to jump to it.

Microphone and camera state are inferred from the current Teams button names;
hand state comes from the hand button's native `FullDescription`. For example,
`Mute mic` means the microphone is live and `Unmute mic` means Teams is muted.
WASAPI is still sampled to discover the Teams audio
process that leads to the right hosted meeting window.

Live Teams state is read every second during a meeting, every 3 seconds while
Teams runs without a meeting, and every 5 seconds while Teams is not running.
Device button presses still refresh Teams immediately. Virtual desktops are
read every second, and the calendar every 30 seconds.

## Build

```powershell
dotnet build .\X3LaptopCompanion.csproj
```

## Hand-state regression checks

From the repository root, run the dependency-free checks with the .NET SDK:

```powershell
dotnet run --project .\laptop_host.tests\X3LaptopCompanion.Tests.csproj --configuration Release
```

To also verify native description reads and repeated meeting snapshots, append
`-- --live <Teams audio PID> <Teams window PID>`. This requires an active meeting
and only reads UI Automation; it never invokes a meeting control.

The same checks cover meeting-window boundaries, unaccepted invitations, safe
join-link validation, stale list selections, UTF-8 payloads, BLE fragmentation,
and pre-join media state parsing. Append `-- --calendar` to also perform a live,
read-only Outlook calendar query and print only the meeting count.

Run the calendar sidecar's fixture checks without Outlook access:

```powershell
powershell.exe -NoProfile -NonInteractive -File .\laptop_host.tests\OutlookCalendar.Tests.ps1
```

## Meeting-list protocol

The host and firmware must both be updated: protocol version **5** adds the
meeting-list characteristic (`...00000d`) and extends button events from 10 to
12 bytes. The final two little-endian bytes carry the displayed meeting-page
revision; join requests also carry a row index in the existing argument byte.
The host rejects stale revisions rather than joining a different meeting after
a refresh. No join URLs are sent to the device.

A meeting page contains `[revision:u16][page:u16][pageCount:u16][status:u8]`
`[count:u8][messageBytes:u8][UTF-8 message]`, followed by up to four entries of
`[startMinutes:u16][endMinutes:u16][titleBytes:u8][UTF-8 title]`. Times are
minutes after local midnight; status is ready (0), joining (1), or error (2).
Messages are at most 80 bytes and titles 64 bytes, truncated at UTF-8 boundaries.
Pages are fragmented to the negotiated ATT payload with
`[version:u8][offset:u16][totalBytes:u16][data]` and written with responses.
The device validates bounds and commits only a fully reassembled page.

## Run

From the repository root, run `.\launch.ps1` to pull updates, build, and launch
the host directly through `dotnet`. Press `Ctrl+C` in that terminal to request
graceful shutdown: the app stops its BLE service and status timer, removes its
tray icon, and exits through the same path as the tray menu's `Exit` command.
The launcher does not kill the app.
Closing the status window only hides it; it does not exit the app.

To launch an existing build from this directory without pulling or rebuilding:

```powershell
dotnet .\bin\Debug\net10.0-windows10.0.19041.0\X3LaptopCompanion.dll
```

Launching the `.exe` from Explorer still runs as a tray app without a console;
use the tray menu's `Exit` command in that case.
