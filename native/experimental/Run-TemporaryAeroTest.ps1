# Authorized temporary experiment. Always restores exact backed-up app binaries.
# Explorer restarts at both switch points; open File Explorer windows can close.
# Any Settings host that loaded the candidate is also closed during restoration.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
$backup = Join-Path $root 'backups\before-aero-experiment-20261014'
$candidate = Join-Path $root 'native\out\aero-prototype'
$installed = Join-Path $env:LOCALAPPDATA 'TransparencyTweak\app'
$results = Join-Path $candidate 'temporary-test-results.json'
$key = 'HKCU:\Software\TransparencyTweak'
$session = (Get-Process -Id $PID).SessionId
$experimentHash = (Get-FileHash -LiteralPath (Join-Path $candidate 'TweakTap.dll')).Hash
$binaryNames = @('TweakTap.dll','TransparencyTweak.exe')
foreach ($name in $binaryNames) {
    foreach ($directory in @($backup,$candidate,$installed)) {
        if (-not (Test-Path -LiteralPath (Join-Path $directory $name))) { throw "Required binary missing: $directory\$name" }
    }
    if ((Get-FileHash -LiteralPath (Join-Path $backup $name)).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $installed $name)).Hash) {
        throw "Installed $name no longer matches the prepared backup. Nothing was changed."
    }
}
$original = Get-ItemProperty -LiteralPath $key
$savedProperties = @{}
foreach ($name in @('AeroGlass','TaskbarTransparency')) {
    $property = $original.PSObject.Properties[$name]
    $savedProperties[$name] = @{Exists=($null -ne $property);Value=if($property){$property.Value}else{$null}}
}
$savedProperties | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $backup 'temporary-test-settings.json') -Encoding UTF8
$record = [ordered]@{StartUTC=[DateTime]::UtcNow.ToString('o');CandidateLoaded=$false;MeasurementPath=$null;TestError=$null;Restored=$false;RestoreError=$null}

function Restore-Registry {
    foreach ($name in $savedProperties.Keys) {
        $savedProperty = $savedProperties[$name]
        if ($savedProperty.Exists) { Set-ItemProperty -LiteralPath $key -Name $name -Value $savedProperty.Value -Type DWord }
        else { Remove-ItemProperty -LiteralPath $key -Name $name -ErrorAction SilentlyContinue }
    }
}

function Restart-AndLoad([string]$directory) {
    $oldHosts = @(Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session)
    $oldIds = @($oldHosts.Id)
    if ($oldHosts.Count) { $oldHosts | Stop-Process -Force }
    Start-Sleep -Seconds 2
    $newHosts = @(Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session)
    if (-not $newHosts.Count) {
        Start-Process -FilePath (Join-Path $env:WINDIR 'explorer.exe') -WindowStyle Hidden
    }
    $injector = Start-Process -FilePath (Join-Path $directory 'TransparencyTweak.exe') -WindowStyle Hidden -PassThru
    if (-not $injector.WaitForExit(60000)) { $injector.Kill(); throw 'Injector timeout.' }
    if ($injector.ExitCode -ne 0) { throw ('Injector failed: ' + $injector.ExitCode) }
    Start-Sleep -Seconds 3
    $currentHosts = @(Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session)
    $dllHash = (Get-FileHash -LiteralPath (Join-Path $directory 'TweakTap.dll')).Hash
    $found = @($currentHosts | Where-Object {
        @($_.Modules | Where-Object ModuleName -like 'TweakTap*' | Where-Object {
            (Get-FileHash -LiteralPath $_.FileName).Hash -eq $dllHash
        }).Count -eq 1
    })
    if ($found.Count -ne 1) { throw 'Expected one Explorer host with the requested DLL hash.' }
    Write-Host ('Loaded requested DLL into Explorer PID ' + $found[0].Id + ' (previous: ' + ($oldIds -join ',') + ')')
    return $found[0].Id
}

try {
    # Start candidate in plain mode to isolate its first transition to glass.
    Set-ItemProperty -LiteralPath $key -Name AeroGlass -Value 0 -Type DWord
    Set-ItemProperty -LiteralPath $key -Name TaskbarTransparency -Value 70 -Type DWord
    foreach ($name in $binaryNames) { Copy-Item -LiteralPath (Join-Path $candidate $name) -Destination (Join-Path $installed $name) -Force }
    $candidatePid = Restart-AndLoad $installed
    $record.CandidateLoaded = $true
    $record.CandidateExplorerPID = $candidatePid
    $measurement = Join-Path $candidate 'taskbar-memory.json'
    & (Join-Path $PSScriptRoot 'Measure-AeroTaskbar.ps1') -Samples 10 -WarmupSeconds 3 -TransparencyPercent 70 -OutputPath $measurement
    $record.MeasurementPath = $measurement
    $log = Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA 'TransparencyTweak\logs') -Filter "Explorer-$candidatePid-*.log" |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($log) {
        Copy-Item -LiteralPath $log.FullName -Destination (Join-Path $candidate 'candidate-explorer.log') -Force
        $logText = Get-Content -LiteralPath $log.FullName -Raw
        $record.CompositionConnected = $logText.Contains('glass composition connected')
        if ($logText -match 'glass .*failed hr=|dispatcher rejected|dispatcher unavailable') { throw 'Candidate logged a rendering or lifecycle failure.' }
        if (-not $record.CompositionConnected) { throw 'No XAML composition connection was observed.' }
    }
} catch {
    $record.TestError = $_.Exception.Message
    Write-Warning $record.TestError
} finally {
    try {
        foreach ($name in $binaryNames) { Copy-Item -LiteralPath (Join-Path $backup $name) -Destination (Join-Path $installed $name) -Force }
        Restore-Registry
        $restoredPid = Restart-AndLoad $installed
        # The existing Settings watcher may also have loaded the candidate.
        # That process must exit to remove pinned callbacks/timers safely.
        $affectedSettings = @(Get-Process SystemSettings -ErrorAction SilentlyContinue |
            Where-Object SessionId -eq $session | Where-Object {
                @($_.Modules | Where-Object ModuleName -like 'TweakTap*' | Where-Object {
                    (Get-FileHash -LiteralPath $_.FileName).Hash -eq $experimentHash
                }).Count -gt 0
            })
        $record.SettingsProcessesClosedForCleanup = @($affectedSettings.Id)
        if ($affectedSettings.Count) { $affectedSettings | Stop-Process -Force }
        foreach ($name in $binaryNames) {
            if ((Get-FileHash -LiteralPath (Join-Path $installed $name)).Hash -ne
                (Get-FileHash -LiteralPath (Join-Path $backup $name)).Hash) { throw "Restored $name hash mismatch." }
        }
        $now = Get-ItemProperty -LiteralPath $key
        foreach ($name in $savedProperties.Keys) {
            $property = $now.PSObject.Properties[$name]
            if (($null -ne $property) -ne $savedProperties[$name].Exists) { throw "Restored $name existence mismatch." }
            if ($property -and $property.Value -ne $savedProperties[$name].Value) { throw "Restored $name value mismatch." }
        }
        $record.Restored = $true
        $record.RestoredExplorerPID = $restoredPid
        Write-Host 'PASS: exact installed DLL/injector and registry values restored.'
    } catch {
        $record.RestoreError = $_.Exception.Message
        Write-Warning ('RESTORE FAILED: ' + $record.RestoreError)
    }
    $record.EndUTC = [DateTime]::UtcNow.ToString('o')
    $record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $results -Encoding UTF8
}
if (-not $record.Restored) { throw ('Restore failed; inspect ' + $results) }
if ($record.TestError) { throw ('Experiment failed, stable state restored; inspect ' + $results) }
Write-Host "Temporary experiment complete; results in $results"
