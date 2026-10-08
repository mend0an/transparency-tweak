# Installs Transparency Tweak for the current user (no administrator rights needed).
#   powershell -ExecutionPolicy Bypass -File install.ps1
#
# Nothing in C:\Windows or Program Files is touched. Everything lives in:
#   %LOCALAPPDATA%\TransparencyTweak            program files and logs
#   HKCU\Software\TransparencyTweak             TaskbarTransparency (0-100 %), set by the slider in Settings
#   Task Scheduler \TransparencyTweak           runs TransparencyTweak.exe at sign-in and unlock; it loads the
#                                               DLL into explorer (if not already there) and exits immediately
# Undo everything with uninstall.ps1.
$ErrorActionPreference = 'Stop'

$build = Join-Path $PSScriptRoot 'native\out'
foreach ($file in 'TweakTap.dll', 'TransparencyTweak.exe') {
    if (-not (Test-Path (Join-Path $build $file))) { throw "$file not found - run native\build.ps1 first" }
}

$app = Join-Path $env:LOCALAPPDATA 'TransparencyTweak\app'
New-Item -ItemType Directory -Force $app | Out-Null
Copy-Item (Join-Path $build 'TweakTap.dll'), (Join-Path $build 'TransparencyTweak.exe') $app -Force

$key = 'HKCU:\Software\TransparencyTweak'
if (-not (Test-Path $key)) { New-Item $key | Out-Null }
if ($null -eq (Get-ItemProperty $key).TaskbarTransparency) {
    Set-ItemProperty $key TaskbarTransparency 0 -Type DWord
}

$user = "$env:USERDOMAIN\$env:USERNAME"
$action = New-ScheduledTaskAction -Execute (Join-Path $app 'TransparencyTweak.exe')
$atLogon = New-ScheduledTaskTrigger -AtLogOn -User $user
# Explorer restarts drop the DLL; unlocking the PC loads it again (a no-op when it is still there).
$unlockClass = Get-CimClass -Namespace Root/Microsoft/Windows/TaskScheduler -ClassName MSFT_TaskSessionStateChangeTrigger
$atUnlock = New-CimInstance -CimClass $unlockClass -ClientOnly -Property @{ StateChange = 8; UserId = $user }
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName 'TransparencyTweak' -Action $action -Trigger @($atLogon, $atUnlock) `
    -Principal $principal -Settings $settings -Force `
    -Description 'Loads the taskbar transparency tweak into explorer.exe, then exits.' | Out-Null

# A previously injected DLL cannot be replaced in memory. Without a host restart
# the old Explorer watcher would keep injecting the crash-prone Settings DLL.
$sessionId = (Get-Process -Id $PID).SessionId
$stamp = (Get-Item (Join-Path $build 'TweakTap.dll')).LastWriteTimeUtc.ToFileTimeUtc()
$expectedModule = 'TweakTap-{0:X8}{1:X8}.dll' -f [uint32]($stamp -shr 32), [uint32]($stamp -band 0xFFFFFFFFL)
$oldHosts = @(Get-Process explorer,SystemSettings -ErrorAction SilentlyContinue |
    Where-Object SessionId -eq $sessionId | Where-Object {
        $oldModules = @($_.Modules | Where-Object {
            $_.ModuleName -match '^TweakTap(?:-.*)?\.dll$' -and $_.ModuleName -ne $expectedModule
        })
        $oldModules.Count -gt 0
    })
if ($oldHosts.Count -gt 0) {
    $oldHosts | Where-Object ProcessName -eq 'SystemSettings' | Stop-Process -Force
    $oldExplorer = @($oldHosts | Where-Object ProcessName -eq 'explorer')
    if ($oldExplorer.Count -gt 0) {
        $oldExplorer | Stop-Process -Force
        Start-Sleep -Seconds 2
        if (-not (Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $sessionId)) {
            Start-Process explorer.exe -WindowStyle Hidden
        }
    }
}

Start-ScheduledTask -TaskName 'TransparencyTweak'
Write-Host 'Installed. Open Settings > Personalization > Colors and use "Taskbar transparency".'
