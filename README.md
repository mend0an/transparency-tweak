# Transparency Tweak

Add taskbar appearance controls directly to **Windows Settings > Personalization > Colors**. Choose the stable plain tint edition, or **Aero Glass 0.1.0-preview.5** for adjustable blur, reflections, and shadows behind system tray icons and the clock/date. Changes apply immediately.

Built for **Windows 11 x64**. Tested on Windows build **26300.9457**; compatibility with other builds has not been verified.

## Preview

### Aero Glass Preview 5 taskbar

![Aero Glass Preview 5 taskbar with blur and shadows behind system tray icons and the clock/date](docs/images/aero-preview5-taskbar.png)

### Aero controls in Windows Settings

![Aero Glass Preview 5 controls in Personalization > Colors: transparency 73%, Aero Glass on, blur strength 24%, reflection strength 40%](docs/images/aero-preview5-settings.png)

These screenshots show **Aero Glass Preview 5**, with **73% taskbar transparency**, **24% Blur strength**, **40% Reflection strength**, and Windows transparency effects enabled. The wallpaper and desktop apps shown are illustrative and are not included.

## Features

- A 0-100% slider integrated into the Windows Colors settings page.
- Immediate taskbar updates, with the selected percentage saved for the current user.
- Automatic loading at sign-in and unlock.
- Per-user installation without administrator rights.
- An uninstaller that restores the default Windows taskbar.
- In Aero Preview 5: adjustable blur/reflections, system tray shadows, and corrected clock shadow sizing for right-aligned text.

## Install or update

### Aero Glass Preview 5

1. Download the installer or ZIP from the [Preview 5 release](https://github.com/mend0an/transparency-tweak/releases/tag/v0.1.0-preview.5), or use the [installer in this repository](dist/TransparencyTweak-Aero-0.1.0-preview.5/Install%20Transparency%20Tweak%20Aero%200.1.0-preview.5.exe).
2. Run **Install Transparency Tweak Aero 0.1.0-preview.5.exe**. No prior uninstall is required. Explorer restarts, so open File Explorer windows may close.
3. Reopen **Settings > Personalization > Colors**, enable **Transparency effects**, then adjust **Taskbar transparency**, **Aero Glass**, **Blur strength**, and **Reflection strength**.

In Aero mode, **100% transparency keeps blur and reflections**; **0% restores the native taskbar brushes**. Shadows follow the shape of tray text/image icons and the clock/date. They are removed at 0% or when Windows transparency effects are off.

Previous program files are preserved. Run the included **Restore Previous Version.exe** to return to the saved version and preferences. See [release notes](docs/AERO-RELEASE.md) for installation details and validation.

### Stable plain tint edition

1. Download [Install Transparency Tweak.exe](dist/Install%20Transparency%20Tweak.exe) using GitHub's **Download raw file** button, or download and extract this repository using **Code > Download ZIP**.
2. Run the installer and accept its confirmation.
3. Open **Settings > Personalization > Colors**.
4. Turn on **Transparency effects**, then adjust **Taskbar transparency**.

Updating an active older version closes Settings if it uses that version and restarts Explorer when necessary. Open File Explorer windows may close. Your transparency percentage is preserved.

## How the stable plain tint slider works

| Setting | Result |
| --- | --- |
| 0% | Restore the native taskbar brushes. |
| 1-99% | Apply a plain taskbar tint with decreasing opacity. |
| 100% | Make the replacement taskbar fill and border fully transparent. |
| Windows Transparency effects off | Restore the native taskbar brushes regardless of the slider value. |

The replacement uses a plain tint instead of native Acrylic blur. It changes the taskbar background; taskbar icons remain visible.

## Uninstall

To roll back an Aero preview, use its **Restore Previous Version.exe**. The full uninstaller below removes the application, including saved Aero version folders.

Download and run [Uninstall Transparency Tweak.exe](dist/Uninstall%20Transparency%20Tweak.exe), then accept its confirmation. Explorer restarts, so open File Explorer windows will close.

If you already have the source files, you can also run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1
```

## Build from source

Requires **Visual Studio C++ Build Tools (x64)** with the C++ workload and **Windows SDK**.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\build.ps1
```

This produces the DLL and injector in `native/out/`, plus standalone installer and uninstaller executables in `dist/`. To install from the source tree after building:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
```

## Files and settings

- Program files: `%LOCALAPPDATA%\TransparencyTweak\app`
- Aero Preview 5: `%LOCALAPPDATA%\TransparencyTweak\versions\aero-0.1.0-preview.5`
- Logs: `%LOCALAPPDATA%\TransparencyTweak\logs`
- Registry: `HKCU\Software\TransparencyTweak\TaskbarTransparency`
- Scheduled task: `TransparencyTweak`

The tweak loads a DLL into Explorer and Settings to customize their UI. It does not modify files in Windows system folders. Because it depends on Windows UI internals, Windows updates may affect compatibility.

See [development notes](docs/DEVELOPMENT.md) for the Settings crash fix, implementation details, and regression test commands.

Aero Preview 5 passed the XAML shadow regression probe, the 20,000-update glass resource probe, package checksum/installer simulation checks, and live clock-shadow update checks. The user confirmed the corrected clock/date/icon shadows and supplied the screenshots above. This is a preview tested on one Windows build; see [tray shadow implementation notes](docs/TRAY-SHADOW.md) for test details and limits.

## Support the project

If you find Transparency Tweak useful, consider giving this repository a star on GitHub or supporting its development with an ETH donation. Every bit of support is appreciated!

**ETH wallet:** `0x60adbf13961c97b1afb317e2fa87531e5be78970`

Thank you for using Transparency Tweak and helping it grow.
