# Removes Transparency Tweak and returns the taskbar to exactly what Windows draws.
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1
#
# Also the emergency restore: if the taskbar or Settings ever look wrong, run this.
# Restarting explorer closes open File Explorer windows; other apps are not affected.
$key = 'HKCU:\Software\TransparencyTweak'

# 1. Back to 0 %: the loaded DLL immediately restores the original opacity and backdrop.
if (Test-Path $key) {
    Set-ItemProperty $key TaskbarTransparency 0 -Type DWord
    Start-Sleep -Seconds 1
}

# 2. Stop loading it at sign-in / unlock.
Unregister-ScheduledTask -TaskName 'TransparencyTweak' -Confirm:$false -ErrorAction SilentlyContinue

# 3. Unload the DLL from memory: the only places it lives are explorer.exe and SystemSettings.exe.
Get-Process SystemSettings -ErrorAction SilentlyContinue | Stop-Process -Force
Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe }
Start-Sleep -Seconds 2

# 4. Delete files and settings.
Remove-Item (Join-Path $env:LOCALAPPDATA 'TransparencyTweak') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $key -Recurse -Force -ErrorAction SilentlyContinue

$left = @()
if (Test-Path (Join-Path $env:LOCALAPPDATA 'TransparencyTweak')) { $left += '%LOCALAPPDATA%\TransparencyTweak' }
if (Get-ScheduledTask -TaskName 'TransparencyTweak' -ErrorAction SilentlyContinue) { $left += 'scheduled task' }
if ($left) { Write-Warning ("Could not remove: " + ($left -join ', ')) }
else { Write-Host 'Transparency Tweak removed. The taskbar is back to the Windows default.' }
