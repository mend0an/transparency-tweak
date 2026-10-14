# Transparency Tweak Aero 0.1.0-preview.1

Separate Aero preview for Windows 11 x64; runtime tested on build 26300.9457.
This package preserves previous program files. Only one version runs at a time.

1. Run **Install Transparency Tweak Aero 0.1.0-preview.1.exe**. No prior uninstall is required.
2. Explorer restarts; File Explorer and Settings windows using the tweak may close.
3. Reopen Settings > Personalization > Colors > Transparency effects.
4. Use Aero Glass, Blur strength, Reflection strength, and Taskbar transparency.
   At 100% transparency, blur and reflections remain; 0% restores native brushes.

Installed files: %LOCALAPPDATA%\TransparencyTweak\versions\aero-0.1.0-preview.1
The original app folder is kept. The existing TransparencyTweak startup task
switches to this version; its previous XML and settings are saved locally.

Run **Restore Previous Version.exe** to return to the saved previous version/settings. Aero
files are retained. If this was a fresh install, restore returns to Windows defaults.
Restore refuses to overwrite another version's startup configuration or a changed
previous DLL. The previous version may itself be an earlier Aero prototype.

Do not run the original uninstaller to switch versions: it removes the entire
TransparencyTweak directory, including version folders. Use it only for full removal.

The standalone EXE files include their scripts/payloads. PowerShell alternatives:

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1

The restore operation switches versions; it does not delete the Aero package.
Preview limitation: exact Windows 7 appearance and total blur/GPU allocation are
not established. Settings controls and this version-switch installer still need
live UI/install validation. Building the package does not activate it.
