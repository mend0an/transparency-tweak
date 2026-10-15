# Exercises the installed candidate's registry -> Explorer -> tray shadow path.
# Saves and restores the optional preference; does not restart or inject Explorer.
[CmdletBinding()]
param([string]$CandidateDll, [switch]$WaitForClockTick)
$ErrorActionPreference = 'Stop'
if (-not $CandidateDll) { $CandidateDll = Join-Path $PSScriptRoot '..\out\aero-prototype\TweakTap.dll' }
$key = 'HKCU:\Software\TransparencyTweak'
$values = Get-ItemProperty -LiteralPath $key
if ($values.TaskbarTransparency -le 0 -or
    (Get-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize').EnableTransparency -eq 0) {
    throw 'This test requires an active transparency level and Windows transparency effects.'
}
$session = (Get-Process -Id $PID).SessionId
$hash = (Get-FileHash -LiteralPath $CandidateDll).Hash
$hosts = @(Get-Process explorer | Where-Object SessionId -eq $session | Where-Object {
    $modules = @($_.Modules | Where-Object ModuleName -like 'TweakTap*')
    $modules.Count -eq 1 -and (Get-FileHash -LiteralPath $modules[0].FileName).Hash -eq $hash
})
if ($hosts.Count -ne 1) { throw 'Expected exactly one Explorer with the candidate DLL. No preferences changed.' }
$log = Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA 'TransparencyTweak\logs') -Filter "Explorer-$($hosts[0].Id)-*.log" |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $log) { throw 'Candidate Explorer log missing. No preferences changed.' }
$property = $values.PSObject.Properties['TrayContentShadow']
$saved = if ($property) { $property.Value } else { $null }
$initialLength = (Get-Content -LiteralPath $log.FullName -Raw).Length
try {
    Set-ItemProperty -LiteralPath $key -Name TrayContentShadow -Type DWord -Value 0
    Start-Sleep -Milliseconds 500
    $beforeEnable = (Get-Content -LiteralPath $log.FullName -Raw).Length
    Set-ItemProperty -LiteralPath $key -Name TrayContentShadow -Type DWord -Value 1
    $updates = ''
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        Start-Sleep -Milliseconds 250
        $updates = (Get-Content -LiteralPath $log.FullName -Raw).Substring($beforeEnable)
        if ($updates -match 'tray shadow applied .*name=TimeInnerTextBlock ' -and
            $updates -match 'tray shadow applied .*name=DateInnerTextBlock ') { break }
    }
    foreach ($name in 'TimeInnerTextBlock','DateInnerTextBlock','InnerTextBlock') {
        if ($updates -notmatch "tray shadow applied .*name=$name ") { throw "No fresh shadow mask for $name after reenable." }
    }
    $clockMask = @($updates -split "`r?`n" | Where-Object { $_ -match 'tray shadow applied .*name=TimeInnerTextBlock .*ink=' }) | Select-Object -Last 1
    if (-not $clockMask -or $clockMask -notmatch 'size=(\d+)x(\d+) ink=([\d.]+)x([\d.]+)') {
        throw 'Clock mask log is missing arranged/ink dimensions.'
    }
    if ([double]$Matches[1] + 0.5 -lt [double]$Matches[3]) { throw 'Clock mask is smaller than its ink.' }
    Write-Output ('PASS: clock mask dimensions: ' + $clockMask)
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        $updates = (Get-Content -LiteralPath $log.FullName -Raw).Substring($beforeEnable)
        $positions = @($updates -split "`r?`n" | Where-Object { $_ -match 'tray shadow positioned name=TimeInnerTextBlock .*host=([1-9]\d*)x([1-9]\d*)' })
        if ($positions.Count) { break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $positions.Count) { throw 'Clock shadow never obtained nonzero host bounds after layout.' }
    Write-Output ('PASS: clock shadow has arranged bounds: ' + $positions[-1])
    if ($WaitForClockTick) {
        $beforeTick = (Get-Content -LiteralPath $log.FullName -Raw).Length
        $deadline = [DateTime]::UtcNow.AddSeconds(65)
        $tick = ''
        do {
            Start-Sleep -Milliseconds 500
            $tickUpdates = (Get-Content -LiteralPath $log.FullName -Raw).Substring($beforeTick)
            $tick = @($tickUpdates -split "`r?`n" | Where-Object { $_ -match 'tray shadow applied .*name=TimeInnerTextBlock .*host=([1-9]\d*)x([1-9]\d*)' })
        } while (-not $tick.Count -and [DateTime]::UtcNow -lt $deadline)
        if (-not $tick.Count) { throw 'No clock mask refresh observed within 65 seconds.' }
        Write-Output ('PASS: natural clock tick refreshed its shadow mask: ' + $tick[-1])
    }
    $allUpdates = (Get-Content -LiteralPath $log.FullName -Raw).Substring($initialLength)
    if ($allUpdates -match 'tray shadow .*failed|tray shadow .*rejected|glass .*failed') {
        throw "Candidate reported failure: $allUpdates"
    }
    Get-Process -Id $hosts[0].Id -ErrorAction Stop | Out-Null
    Write-Output 'PASS: live shadow disable/enable recreated clock, date and icon masks; Explorer survived without shadow errors.'
    Write-Output "Log: $($log.FullName)"
} finally {
    if ($null -ne $property) { Set-ItemProperty -LiteralPath $key -Name TrayContentShadow -Type DWord -Value $saved }
    else { Remove-ItemProperty -LiteralPath $key -Name TrayContentShadow -ErrorAction SilentlyContinue }
}
Write-Output 'Original shadow preference restored. Visual appearance still requires confirmation.'
