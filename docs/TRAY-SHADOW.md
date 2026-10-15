# Aero Preview 5: system tray shadows and clock mask sizing

Adds a black shadow behind TextBlock and Image content inside SystemTrayFrame,
including the clock, date, notification icons, and font icons rendered as text.
The foreground colors, opacity, click targets, and glass renderer are unchanged.
Other taskbar buttons and notification/overflow flyouts are outside the scope.

The shadow uses the element's GetAlphaMask, a 2.5 DIP blur, 85% opacity, and a
1 DIP downward offset. A Canvas inserted before the foreground in the nearest
Grid hosts the shadow visual. It measures to zero (no added desired layout
space), but stretches across the Grid's rows and columns when arranged.
No Explorer-owned Composition child visual is replaced. Preview 3 used an
explicitly zero-sized host; Preview 4 changes that layout after the user
reported missing clock shadow despite successful clock-mask logs.
Preview 5 corrects mask dimensions: TextBlock.ActualWidth/ActualHeight report
ink extents, which can be much narrower than its arranged mask area. Using
those extents compressed and displaced the right-aligned time shadow in the
user's screenshot. UIElement.ActualSize now supplies the shadow visual size.
Zero-size sources hide their existing shadow until they are arranged again.
Layout changes reposition the shadow; text/image/size changes refresh its mask.
Resources are reused between updates and released when the element is removed.
No screenshots, texture polling, window focus hooks, or new runtime dependencies.

Shadows are active when Taskbar transparency is above 0% and Windows transparency
effects are on, in both Aero Glass and plain tint modes of the Aero build.
An optional DWORD `TrayContentShadow=0` under `HKCU\Software\TransparencyTweak`
disables shadows. Missing means enabled; no additional Settings control is added.
The existing registry watcher applies changes without restarting Explorer.

## Verification

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\experimental\build-aero-prototype.ps1
.\native\out\aero-prototype\TrayShadowProbe.exe
.\native\out\aero-prototype\AeroGlassProbe.exe
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\tests\TrayContentShadow.ps1
# Also verify the next natural minute update (up to 65 seconds):
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\tests\TrayContentShadow.ps1 -WaitForClockTick
```

TrayShadowProbe uses a hidden XAML island in its own process. It checks real
TextBlock/Image alpha masks, placement behind foreground, deduplication, clock
and image updates without replacing visuals, disable/enable, native mode,
scope exclusion, and removal/Stop cleanup. It does not inject Explorer or
change registry. A regression case adds separate right-aligned clock/date rows
and a StackPanel wrapper, checks nonzero host bounds and unchanged text
positions, and exercises hide/show and minute updates. The glass probe checks
reuse over 20,000 parameter updates.
These checks do not prove the appearance of pixels on the real taskbar.

The Preview 5 regression stretches the time TextBlock and right-aligns its
text, reproducing the distinction between ink width and mask layout width.
With the old implementation it failed: ink=29, arranged=96, shadow=29 DIP.
With ActualSize it passed: ink=29, arranged=96, shadow=96 DIP. It also covers
collapse/restore and clock updates without recreating the shadow visual.
The active tray clock reported an arranged mask about 63x16 DIP with ink
29x16 DIP after Preview 5 activation.
The user confirmed the corrected appearance on 15 October 2026 and supplied
the [taskbar screenshot](images/aero-preview5-taskbar.png) and
[Settings screenshot](images/aero-preview5-settings.png) shown in the README.
This is manual confirmation on the tested Windows build, not automated pixel
verification or a claim of compatibility with other builds.
The live Preview 5 toggle check passed, and the natural minute update at
21:02 WIB refreshed the 63x16 DIP mask while preserving the user's preferences.

References: [TextBlock ink-size behavior](https://github.com/microsoft/microsoft-ui-xaml/issues/8804),
[UIElement.ActualSize](https://learn.microsoft.com/en-us/uwp/api/windows.ui.xaml.uielement.actualsize).

The live PowerShell test requires the candidate DLL already loaded in Explorer.
It temporarily toggles only TrayContentShadow, checks new clock/date/icon mask
logs and host survival, then restores the optional preference exactly. This
test passed on the active Preview 3 installation on 15 October 2026. The
Preview 4 check additionally verifies nonzero arranged clock-host bounds; the
live host measured 62x40 DIP, and a natural minute refresh at 20:53 WIB passed.
Transparency
73%, blur sigma 3.84 and reflection 12% were preserved during installation.

Build the versioned candidate and check installer simulation with:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\experimental\build-aero-package.ps1 -Version 0.1.0-preview.5
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\experimental\Test-AeroPackage.ps1 -Version 0.1.0-preview.5
```

Preview 1/2 packages and the stable plain-tint installer remain available.
