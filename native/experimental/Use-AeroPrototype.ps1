# Enable the experimental taskbar renderer, or restore the saved stable build.
# Explorer and Settings hosts with TweakTap are restarted/closed during switching.
[CmdletBinding()]
param(
    [ValidateSet('Enable','Restore')][string]$Mode = 'Enable',
    [ValidateRange(1,100)][int]$TransparencyPercent = 70
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
$candidate = Join-Path $root 'native\out\aero-prototype'
$backup = Join-Path $root 'backups\before-aero-experiment-20261014'
$installed = Join-Path $env:LOCALAPPDATA 'TransparencyTweak\app'
$statePath = Join-Path $candidate 'activation-state.json'
$key = 'HKCU:\Software\TransparencyTweak'
$personalize = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
$session = (Get-Process -Id $PID).SessionId
$names = @('TweakTap.dll','TransparencyTweak.exe')
foreach ($name in $names) {
    foreach ($directory in @($candidate,$backup,$installed)) {
        if (-not (Test-Path -LiteralPath (Join-Path $directory $name))) { throw "Required binary missing: $directory\$name" }
    }
}
$candidateHash = (Get-FileHash -LiteralPath (Join-Path $candidate 'TweakTap.dll')).Hash
$stableHash = (Get-FileHash -LiteralPath (Join-Path $backup 'TweakTap.dll')).Hash
$currentHash = (Get-FileHash -LiteralPath (Join-Path $installed 'TweakTap.dll')).Hash
$previousActiveHash = $null
$verificationPath = Join-Path $candidate 'active-verification.json'
if (Test-Path -LiteralPath $verificationPath) {
    $previousActiveHash = (Get-Content -LiteralPath $verificationPath -Raw | ConvertFrom-Json).DLLSHA256
}
if ($currentHash -ne $candidateHash -and $currentHash -ne $stableHash -and $currentHash -ne $previousActiveHash) {
    throw 'Installed DLL differs from both the reviewed candidate and stable backup. Nothing was changed.'
}
if ($Mode -eq 'Restore' -and -not (Test-Path -LiteralPath $statePath)) { throw 'Activation state missing; cannot restore saved registry values.' }
if (-not (Test-Path -LiteralPath $statePath)) {
    $values = Get-ItemProperty -LiteralPath $key
    $preferences = Get-ItemProperty -LiteralPath $personalize
    $saved = foreach ($name in @('AeroGlass','TaskbarTransparency')) {
        $property = $values.PSObject.Properties[$name]
        [pscustomobject]@{Key=$key;Name=$name;Exists=($null -ne $property);Value=if($property){$property.Value}else{$null}}
    }
    $property = $preferences.PSObject.Properties['EnableTransparency']
    $saved += [pscustomobject]@{Key=$personalize;Name='EnableTransparency';Exists=($null -ne $property);Value=if($property){$property.Value}else{$null}}
    [pscustomobject]@{CreatedUTC=[DateTime]::UtcNow.ToString('o');StableDLLSHA256=$stableHash;SavedValues=@($saved);Active=$false} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $statePath -Encoding UTF8
}
$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
if ($state.StableDLLSHA256 -ne $stableHash) { throw 'Activation state and stable backup disagree.' }

function Write-SavedValues {
    foreach ($value in $state.SavedValues) {
        if ($value.Exists) { Set-ItemProperty -LiteralPath $value.Key -Name $value.Name -Value $value.Value -Type DWord }
        else { Remove-ItemProperty -LiteralPath $value.Key -Name $value.Name -ErrorAction SilentlyContinue }
    }
}

function Load-Build([string]$directory) {
    # Close only Settings hosts that contain this tweak, to avoid mixing builds.
    $settingsHosts = @(Get-Process SystemSettings -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session |
        Where-Object { @($_.Modules | Where-Object ModuleName -like 'TweakTap*').Count -gt 0 })
    if ($settingsHosts.Count) { $settingsHosts | Stop-Process -Force }
    foreach ($name in $names) { Copy-Item -LiteralPath (Join-Path $directory $name) -Destination (Join-Path $installed $name) -Force }
    @(Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session) | Stop-Process -Force
    Start-Sleep -Seconds 2
    if (-not @(Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session).Count) {
        Start-Process -FilePath (Join-Path $env:WINDIR 'explorer.exe') -WindowStyle Hidden
    }
    $process = Start-Process -FilePath (Join-Path $installed 'TransparencyTweak.exe') -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(60000)) { $process.Kill(); throw 'Injector timed out.' }
    if ($process.ExitCode -ne 0) { throw ('Injector failed: ' + $process.ExitCode) }
    Start-Sleep -Seconds 3
    $expected = (Get-FileHash -LiteralPath (Join-Path $directory 'TweakTap.dll')).Hash
    $hosts = @(Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session |
        Where-Object { @($_.Modules | Where-Object ModuleName -like 'TweakTap*' | Where-Object {
            (Get-FileHash -LiteralPath $_.FileName).Hash -eq $expected
        }).Count -eq 1 })
    if ($hosts.Count -ne 1) { throw 'Requested DLL was not found in exactly one Explorer host.' }
    return $hosts[0].Id
}

try {
    if ($Mode -eq 'Enable') {
        Set-ItemProperty -LiteralPath $personalize -Name EnableTransparency -Value 1 -Type DWord
        Set-ItemProperty -LiteralPath $key -Name AeroGlass -Value 1 -Type DWord
        Set-ItemProperty -LiteralPath $key -Name TaskbarTransparency -Value $TransparencyPercent -Type DWord
        $targetPid = Load-Build $candidate
        $log = Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA 'TransparencyTweak\logs') -Filter "Explorer-$targetPid-*.log" |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $log) { throw 'Candidate log unavailable.' }
        $text = Get-Content -LiteralPath $log.FullName -Raw
        if ($text -notmatch 'BackgroundFill .*glass=1' -or -not $text.Contains('glass composition connected')) {
            throw 'Glass renderer did not confirm a connected brush.'
        }
        if ($text -match 'glass .*failed hr=|dispatcher rejected|dispatcher unavailable') { throw 'Candidate reported a rendering failure.' }
        $state.Active = $true
        $state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $statePath -Encoding UTF8
        [pscustomobject]@{Mode='glass';ExplorerPID=$targetPid;DLLSHA256=$candidateHash;TransparencyPercent=$TransparencyPercent;Log=$log.FullName;VisualVerified=$false} |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $candidate 'active-verification.json') -Encoding UTF8
        Write-Host "Glass prototype is active in Explorer PID $targetPid at $TransparencyPercent% transparency."
        Write-Host 'Visual appearance still needs confirmation; the brush connection alone does not prove visible blur.'
    } else {
        Write-SavedValues
        $targetPid = Load-Build $backup
        $state.Active = $false
        $state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $statePath -Encoding UTF8
        Write-Host "Stable build and saved settings restored in Explorer PID $targetPid."
    }
} catch {
    $failure = $_.Exception.Message
    Write-Warning ('Switch failed: ' + $failure + '. Restoring stable build.')
    Write-SavedValues
    Load-Build $backup | Out-Null
    $state.Active = $false
    $state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $statePath -Encoding UTF8
    throw $failure
}
