# X3 Laptop Companion Host

.NET 10 WPF tray app for the X3 BLE Laptop Companion.

## Requirements

- Windows 10 19041 or newer.
- .NET 10 SDK.
- Bluetooth LE adapter.

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
  process when possible.
- Handles mute, camera, and raise-hand commands by finding the current Teams UI
  Automation button and invoking it with `InvokePattern`. The meeting window is
  cached, but controls are rediscovered for each command because Teams rerenders
  the meeting toolbar.
- Reads the raised-hand state from the `Raise your hand` / `Lower your hand`
  button pair, whose names describe the next action and therefore the current
  state. Newer Teams builds replaced that pair with a single stateless button
  called `Raise` (`raisehands-button`). When only that button is present the
  state cannot be read back, so the host remembers it, toggles it on each press,
  and reports the remembered value. That fallback engages automatically and the
  stateful path is kept intact, so nothing needs changing if Teams restores it.
  The remembered value is a guess: it is seeded to lowered and reset whenever the
  meeting changes, so it can disagree with Teams if the hand is changed elsewhere.
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
- Writes a diagnostic log to `%LOCALAPPDATA%\X3LaptopCompanion\host.log`.
  Use the `Open Log` button or tray menu item to jump to it.

Microphone, camera, and hand state are inferred from the current Teams button
names, for example `Mute mic` means the microphone is live and `Unmute mic`
means Teams is muted. WASAPI is still sampled to discover the Teams audio
process that leads to the right hosted meeting window.

## Build

```powershell
dotnet build .\X3LaptopCompanion.csproj
```
