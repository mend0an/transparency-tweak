# Runtime regression test for build 26300's Background crash. Build first.
# Uses an ordinary DLL bootstrap, or the automatic Explorer watcher with
# -Automatic (the matching Explorer DLL must already be loaded).
[CmdletBinding()]
param(
    [switch]$RestartSettings,
    [switch]$Automatic,
    [ValidateRange(1, 10)][int]$Cycles = 3
)
$ErrorActionPreference = 'Stop'
$sessionId = (Get-Process -Id $PID).SessionId
$injector = Join-Path (Split-Path $PSScriptRoot) 'out\TransparencyTweak.exe'
$logs = Join-Path $env:LOCALAPPDATA 'TransparencyTweak\logs'
$testStart = Get-Date

function Get-Settings {
    Get-Process SystemSettings -ErrorAction SilentlyContinue |
        Where-Object SessionId -eq $sessionId |
        Sort-Object StartTime -Descending | Select-Object -First 1
}

if ($RestartSettings) {
    Get-Process SystemSettings -ErrorAction SilentlyContinue |
        Where-Object SessionId -eq $sessionId | Stop-Process -Force
}
Start-Process 'ms-settings:personalization-colors' -WindowStyle Hidden
Start-Sleep -Seconds 3
$settingsProcess = Get-Settings
if (-not $settingsProcess) { throw 'Settings did not start.' }
$settingsId = $settingsProcess.Id
if (-not $Automatic) {
    $result = Start-Process -FilePath $injector -ArgumentList "$settingsId" -WindowStyle Hidden -Wait -PassThru
    if ($result.ExitCode -ne 0) { throw "Injector failed ($($result.ExitCode)). See $logs\injector.log" }
}

function Get-TapLog {
    $files = Get-ChildItem $logs -Filter "SystemSettings-$settingsId-*.log" -ErrorAction SilentlyContinue
    ($files | ForEach-Object { Get-Content $_.FullName -Raw }) -join "`n"
}

function Assert-Colors {
    Start-Sleep -Seconds 3
    $text = Get-TapLog
    if ($text -notmatch 'settings scanner queued without XAML diagnostics' -or $text -notmatch 'slider attached') {
        throw 'The Settings bootstrap and slider were not confirmed in the log.'
    }
    if ($text -match 'SetSite:|AdviseVisualTreeChange|failed hr=') {
        throw "Unexpected diagnostics session or callback failure:`n$text"
    }
}

Assert-Colors
for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
    Start-Process 'ms-settings:personalization-background' -WindowStyle Hidden
    # Longer than the scanner interval: catch both navigation and scan failures.
    Start-Sleep -Seconds 4
    $alive = Get-Process -Id $settingsId -ErrorAction SilentlyContinue
    if (-not $alive -or $alive.HasExited) { throw "Settings crashed opening Background (cycle $cycle)." }
    Start-Process 'ms-settings:personalization-colors' -WindowStyle Hidden
    Assert-Colors
    $current = Get-Settings
    if (-not $current -or $current.Id -ne $settingsId) { throw 'Settings restarted during navigation.' }
    Write-Host "PASS cycle ${cycle}: Colors -> Background -> Colors, same Settings PID $settingsId"
}
$crashes = Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000; StartTime=$testStart} -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -match 'SystemSettings.exe' }
if ($crashes) { throw 'Windows recorded a Settings crash during the test.' }
Write-Host 'PASS: slider attached, no diagnostics subscription, no Settings crash.'
