# Checks the complete registry -> Explorer -> taskbar brush path. Visual
# confirmation is still required: a successful property setter alone is not
# evidence that Windows displayed the intended pixels.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$key = 'HKCU:\Software\TransparencyTweak'
$personalize = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
if ((Get-ItemProperty $personalize).EnableTransparency -eq 0) {
    throw 'Enable Transparency effects in Colors before running this test.'
}
$sessionId = (Get-Process -Id $PID).SessionId
$explorer = Get-Process explorer | Where-Object SessionId -eq $sessionId | Select-Object -First 1
$logs = Join-Path $env:LOCALAPPDATA 'TransparencyTweak\logs'
$log = Get-ChildItem $logs -Filter "Explorer-$($explorer.Id)-*.log" |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $log) { throw 'No active Explorer tweak log was found. Install the current build first.' }
$saved = (Get-ItemProperty $key).TaskbarTransparency
try {
    foreach ($level in 0,25,50,100) {
        $before = (Get-Content $log.FullName -Raw).Length
        Set-ItemProperty $key TaskbarTransparency $level -Type DWord
        Start-Sleep -Seconds 2
        $updates = (Get-Content $log.FullName -Raw).Substring($before)
        $wanted = if ($level -eq 0) { '-1.000' } else {
            (1.0 - $level / 100.0).ToString('F3', [Globalization.CultureInfo]::InvariantCulture)
        }
        foreach ($name in 'BackgroundFill','BackgroundStroke') {
            if ($updates -notmatch "taskbar brush $name requested=$([regex]::Escape($wanted)) actual=") {
                throw "No live brush update for $name at $level%. See $($log.FullName)"
            }
        }
        if ($level -eq 100 -and $updates -notmatch 'BackgroundFill requested=0\.000 actual=0\.000') {
            throw 'The 100% setting did not make the taskbar fill brush fully transparent.'
        }
        if ($updates -match 'failed hr=|result=0 error=|rejected update|update skipped') {
            throw "Taskbar update failed:`n$updates"
        }
        Write-Host "PASS: $level% registry setting applied to taskbar fill and border brushes"
    }
} finally {
    Set-ItemProperty $key TaskbarTransparency $saved -Type DWord
}
Write-Host 'PASS: live brush updates verified; original slider setting restored. Confirm the visual result separately.'
