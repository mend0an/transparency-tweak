# Experimental animation following the taskbar edge

This optional controller opens Start and tray panels from the physical screen
edge occupied by the taskbar on the panel's monitor. The panel finishes at its
normal Windows position. Closing uses the native Windows animation.

| Taskbar position | Opening motion |
| --- | --- |
| Left | From the left edge, moving right. |
| Top | From the top edge, moving down. |
| Right | From the right edge, moving left. |
| Bottom | From the bottom edge, moving up. |

The primary or secondary Explorer taskbar is detected on each opening.
Auto-hidden taskbars can also be resolved through the per-monitor appbar query.
If no taskbar is detected, the panel retains its native animation. A theme that
only paints a taskbar elsewhere without moving its host may not expose that
position to this detector.

The controller targets these specific shell windows:

- Start menu: `StartMenuExperienceHost.exe` / `Windows.UI.Core.CoreWindow`.
- Quick Settings and Notification Center: `ShellHost.exe` / `ControlCenterWindow`.
- Legacy Notification Center: `ShellExperienceHost.exe` / CoreWindow on its `ActionCenter` thread.
- Hidden tray icons: `explorer.exe` / `TopLevelWindowForOverflowXamlIsland`.

Opening uses a 280 ms cubic ease-out and per-monitor physical coordinates. The
movement timer runs only during a slide. The controller preserves the panel's
final position, size, activation and Z order, and respects Windows' Animation
effects preference. Changing the taskbar edge or monitor geometry during a slide
cancels it. Closing a panel mid-slide or stopping the controller restores its
saved position. Use `--stop`; forcibly killing a moving controller can prevent
restoration.

## Build and try

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\build-flyout-animation.ps1
Start-Process .\native\out\ShellFlyoutAnimation.exe -WindowStyle Hidden
```

Stop an existing controller before rebuilding. To stop it gracefully:

```powershell
.\native\out\ShellFlyoutAnimation.exe --stop
```

The prototype is separate from the stable installer. It does not start at
sign-in, inject a DLL or change registry settings. Logs are written beside the
executable as `ShellFlyoutAnimation.exe.log`. `--diagnose` reports recognized
shell windows without moving them. If no shell targets are accessible on the
current desktop, the controller exits with code 3.

## Validation

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\tests\ShellFlyoutAnimation.ps1 -Integration
```

The build treats compiler warnings as errors. Geometry and target tests cover
all four edges, negative monitor coordinates, scaled panels, auto-hidden taskbar
geometry, easing and isolation from unrelated apps. The Win32 integration test
uses its own temporary window to check all four directions, duplicate opening
events, completion, hiding mid-slide, edge changes, shutdown restoration,
destruction and timer cleanup. It does not move the user's taskbar.

These checks verify positioning and lifecycle behavior. The shell can still
apply its own internal content animation or clipping. Show/uncloak events are
asynchronous, so an initial flash at the original position is possible. The
taskbar's Z order can obscure part of the motion. Visual checks remain necessary
before integrating this prototype into the installed tweak.

Check Start, Quick Settings, notifications/calendar and tray overflow with each
taskbar position, auto-hide, Animation effects off, mixed-DPI monitors, repeated
opens, closing mid-slide and `--stop` mid-slide. Confirm the final position and
normal mouse/keyboard behavior.

API references: [SetWinEventHook](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook),
[event constants](https://learn.microsoft.com/en-us/windows/win32/winauto/event-constants),
[SetWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos),
[Animation effects](https://learn.microsoft.com/en-us/windows/win32/winauto/client-area-animation),
and [ABM_GETAUTOHIDEBAREX](https://learn.microsoft.com/en-us/windows/win32/shell/abm-getautohidebarex).
