# Development notes

## Background-page crash fix

On the tested Windows build **26300.9457**, initializing XAML diagnostics in
`SystemSettings.exe` caused **Personalization > Background** to crash with
`0xc000027b` in `Windows.UI.Xaml.dll`. The crash reproduced even when the slider
and scanner were disabled, and also after a successful visual-tree unsubscribe.
Opening Background in a clean process without diagnostics worked.

Explorer still uses XAML diagnostics for the taskbar. Settings instead loads
the DLL normally and calls `StartSettingsTweak`, which starts the scanner on the
application's UI dispatcher. Settings never creates a diagnostics session.
Both the standalone injector and Explorer's automatic watcher use this path.

## Transparency behavior

The slider writes the percentage to the registry; Explorer replaces the taskbar
fill and border brushes with solid brushes of the corresponding opacity. This
avoids depending on the native Acrylic brush's composition effect. At 100% the
replacement brush has zero opacity. At 0%, or when Windows' Transparency effects
is off, the native brushes are restored. The active replacement uses a plain
tint rather than the native Acrylic blur.

The earlier implementation changed only the rectangle's opacity. Its setters
succeeded, but the user reported no visible transparency. The brush replacement
was subsequently confirmed by the user to visibly change the taskbar.

## Build and regression test

Requires Visual Studio C++ Build Tools (x64) and the Windows SDK.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\build.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\tests\SettingsNavigation.ps1 -RestartSettings -Cycles 3
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\tests\TaskbarTransparency.ps1
```

To test the automatic watcher, run `native/out/TransparencyTweak.exe` to load
the current build into Explorer, then run the same test with `-Automatic`.
An older DLL already loaded into Explorer needs an installer update or an
Explorer restart first.

The test checks that the slider attached, no diagnostics session was started in
Settings, the same Settings process survives repeated Colors/Background
navigation, and no Settings crash was recorded in the Windows Application log.
`-RestartSettings` closes Settings before the test. Logs are written to
`%LOCALAPPDATA%\TransparencyTweak\logs`.

The transparency test temporarily exercises 0%, 25%, 50%, and 100%, checks live
brush update logs, and restores the previous percentage. It verifies the update
path; visual confirmation is a separate check.
