# Measures the installed experimental taskbar renderer. Refuses the stable DLL.
# Changes AeroGlass / TaskbarTransparency temporarily and restores both values.
[CmdletBinding()]
param(
    [ValidateRange(3,60)][int]$Samples = 10,
    [ValidateRange(1,30)][int]$WarmupSeconds = 3,
    [ValidateRange(1,100)][int]$TransparencyPercent = 70,
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\out\aero-prototype\taskbar-memory.json')
)
$ErrorActionPreference = 'Stop'
$key = 'HKCU:\Software\TransparencyTweak'
$personalize = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
if ((Get-ItemProperty -LiteralPath $personalize).EnableTransparency -eq 0) {
    throw 'Windows Transparency effects must be on for this comparison.'
}
$session = (Get-Process -Id $PID).SessionId
$explorer = Get-Process explorer | Where-Object SessionId -eq $session | Select-Object -First 1
if (-not $explorer) { throw 'Explorer for the current session is unavailable.' }
$candidate = Join-Path $PSScriptRoot '..\out\aero-prototype\TweakTap.dll'
if (-not (Test-Path -LiteralPath $candidate)) { throw 'Build the experimental DLL first.' }
$candidateHash = (Get-FileHash -LiteralPath $candidate).Hash
$matching = @($explorer.Modules | Where-Object ModuleName -like 'TweakTap*' | Where-Object {
    (Get-FileHash -LiteralPath $_.FileName).Hash -eq $candidateHash
})
if ($matching.Count -ne 1) { throw 'The experimental DLL is not loaded in Explorer. No settings were changed.' }
$otherTaps = @($explorer.Modules | Where-Object ModuleName -like 'TweakTap*' | Where-Object {
    (Get-FileHash -LiteralPath $_.FileName).Hash -ne $candidateHash
})
if ($otherTaps.Count) { throw 'A second TweakTap DLL is active; an A/B comparison would be invalid.' }
$logs = Join-Path $env:LOCALAPPDATA 'TransparencyTweak\logs'
$log = Get-ChildItem -LiteralPath $logs -Filter "Explorer-$($explorer.Id)-*.log" |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $log) { throw 'Experimental Explorer log unavailable.' }
$saved = Get-ItemProperty -LiteralPath $key
$hadAero = $null -ne $saved.PSObject.Properties['AeroGlass']
$hadPercent = $null -ne $saved.PSObject.Properties['TaskbarTransparency']
$modes = @()
$gpuFailure = $null
try {
    Set-ItemProperty -LiteralPath $key -Name TaskbarTransparency -Value $TransparencyPercent -Type DWord
    foreach ($mode in @(@{Name='plain_tint';Value=0}, @{Name='glass';Value=1})) {
        $logOffset = (Get-Content -LiteralPath $log.FullName -Raw).Length
        Set-ItemProperty -LiteralPath $key -Name AeroGlass -Value $mode.Value -Type DWord
        Start-Sleep -Seconds $WarmupSeconds
        $allLog = Get-Content -LiteralPath $log.FullName -Raw
        $newLog = $allLog.Substring($logOffset)
        # Setting the already active value need not cause a registry change
        # notification. Validate the latest fill state, including initial replay.
        $fillStates = [regex]::Matches($allLog, '(?m)^.*taskbar brush BackgroundFill .*glass=([01]).*$')
        $lastFill = if ($fillStates.Count) { $fillStates[$fillStates.Count-1] } else { $null }
        $requested = (1.0 - $TransparencyPercent / 100.0).ToString('F3',[Globalization.CultureInfo]::InvariantCulture)
        if (-not $lastFill -or $lastFill.Groups[1].Value -ne [string]$mode.Value -or
            $lastFill.Value -notmatch ('requested=' + [regex]::Escape($requested))) {
            throw ('Renderer did not confirm mode ' + $mode.Name)
        }
        if ($newLog -match 'glass .*failed hr=|dispatcher rejected|dispatcher unavailable') {
            throw ('Renderer reported a failure in ' + $mode.Name)
        }
        $rows = @()
        for ($sample = 0; $sample -lt $Samples; $sample++) {
            $hosts = @(Get-Process explorer,dwm,SystemSettings -ErrorAction SilentlyContinue |
                Where-Object SessionId -eq $session)
            $hostIds = @($hosts.Id)
            $gpu = @()
            try {
                $counters = Get-Counter -Counter '\GPU Process Memory(*)\Dedicated Usage','\GPU Process Memory(*)\Shared Usage' -ErrorAction Stop
                foreach ($counter in $counters.CounterSamples) {
                    if ($counter.InstanceName -match '^pid_(\d+)_') {
                        $counterPid = [int]$Matches[1]
                        if ($hostIds -contains $counterPid) {
                            $gpu += [pscustomobject]@{PID=$counterPid;Path=$counter.Path;Bytes=[int64]$counter.CookedValue}
                        }
                    }
                }
            } catch { $gpuFailure = $_.Exception.Message }
            $rows += [pscustomobject]@{
                TimestampUTC = [DateTime]::UtcNow.ToString('o')
                Hosts = @($hosts | ForEach-Object {
                    $_.Refresh()
                    [pscustomobject]@{PID=$_.Id;Name=$_.ProcessName;PrivateCommitBytes=$_.PrivateMemorySize64;WorkingSetBytes=$_.WorkingSet64}
                })
                GPU = $gpu
            }
            Start-Sleep -Seconds 1
        }
        $modes += [pscustomobject]@{Name=$mode.Name;Samples=$rows}
        Write-Host ('Captured ' + $mode.Name + ': ' + $Samples + ' samples')
    }
} finally {
    if ($hadAero) { Set-ItemProperty -LiteralPath $key -Name AeroGlass -Value $saved.AeroGlass -Type DWord }
    else { Remove-ItemProperty -LiteralPath $key -Name AeroGlass -ErrorAction SilentlyContinue }
    if ($hadPercent) { Set-ItemProperty -LiteralPath $key -Name TaskbarTransparency -Value $saved.TaskbarTransparency -Type DWord }
    else { Remove-ItemProperty -LiteralPath $key -Name TaskbarTransparency -ErrorAction SilentlyContinue }
}
$report = [pscustomobject]@{
    TimestampUTC=[DateTime]::UtcNow.ToString('o')
    Scope='Installed experimental taskbar, same Explorer process: warm plain tint then warm glass'
    CandidateSHA256=$candidateHash
    TransparencyPercent=$TransparencyPercent
    GPUCounterFailure=$gpuFailure
    Limitations=@('Host totals include unrelated allocations and activity.','Sequential warm comparison excludes cold-start costs.','GPU counters absent from a sample do not establish zero usage.','Visual Aero equivalence is not tested by memory sampling.')
    Modes=$modes
}
$outputDirectory = Split-Path ([IO.Path]::GetFullPath($OutputPath))
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$report | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Host "Saved $OutputPath; original registry settings restored."
