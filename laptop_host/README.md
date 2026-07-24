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
- Finds the active Teams meeting window from the parent of the Teams audio
  process when possible.
- Handles mute, camera, and raise-hand commands by finding the current Teams UI
  Automation button and invoking it with `InvokePattern`. The meeting window is
  cached, but controls are rediscovered for each command because Teams rerenders
  the meeting toolbar.
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
