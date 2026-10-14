# Versioned Aero installation. Keeps previous binaries; only one startup task.
[CmdletBinding()]
param([ValidateSet('Enable','Previous')][string]$Mode = 'Enable', [switch]$Plan)
$ErrorActionPreference = 'Stop'
$package = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package.json') -Raw | ConvertFrom-Json
if ($package.Channel -ne 'aero' -or $package.Version -notmatch '^\d+\.\d+\.\d+-preview\.\d+$') { throw 'Invalid Aero package identity.' }
$root = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'TransparencyTweak'))
$versionDir = Join-Path $root ('versions\aero-' + $package.Version)
$exe = Join-Path $versionDir 'TransparencyTweak.exe'
$statePath = Join-Path $versionDir 'previous-version.json'
$key = 'HKCU:\Software\TransparencyTweak'
$personalize = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
$session = (Get-Process -Id $PID).SessionId
$task = Get-ScheduledTask -TaskName 'TransparencyTweak' -ErrorAction SilentlyContinue
$currentExe = if ($task) {
    if (@($task.Actions).Count -ne 1 -or $task.Actions[0].Arguments) { throw 'Unexpected startup task; nothing was changed.' }
    [string]$task.Actions[0].Execute
} else { $null }

function Assert-PreviousExecutable([string]$path) {
    if (-not $path) { return }
    $full = [IO.Path]::GetFullPath($path)
    if (-not $full.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($full) -ne 'TransparencyTweak.exe' -or
        -not (Test-Path -LiteralPath $full) -or
        -not (Test-Path -LiteralPath (Join-Path (Split-Path $full) 'TweakTap.dll'))) {
        throw 'Previous program files are missing or outside the tweak directory; nothing was changed.'
    }
}

function Read-Preferences {
    foreach ($entry in @(
        @{Key=$key;Names=@('TaskbarTransparency','AeroGlass','GlassBlurSigmaMilli','GlassReflectionPercent','GlassTintArgb')},
        @{Key=$personalize;Names=@('EnableTransparency')}
    )) {
        $values = Get-ItemProperty -LiteralPath $entry.Key -ErrorAction SilentlyContinue
        foreach ($name in $entry.Names) {
            $property = if ($values) { $values.PSObject.Properties[$name] } else { $null }
            [pscustomobject]@{Key=$entry.Key;Name=$name;Exists=($null -ne $property);Value=if($property){$property.Value}else{$null}}
        }
    }
}

function Restore-Preferences($state) {
    foreach ($value in $state.Preferences) {
        if ($value.Exists) {
            if (-not (Test-Path -LiteralPath $value.Key)) { New-Item -Path $value.Key -Force | Out-Null }
            Set-ItemProperty -LiteralPath $value.Key -Name $value.Name -Value $value.Value -Type DWord
        } else { Remove-ItemProperty -LiteralPath $value.Key -Name $value.Name -ErrorAction SilentlyContinue }
    }
}

function Register-AeroTask {
    $user = "$env:USERDOMAIN\$env:USERNAME"
    $action = New-ScheduledTaskAction -Execute $exe
    $logon = New-ScheduledTaskTrigger -AtLogOn -User $user
    $class = Get-CimClass -Namespace Root/Microsoft/Windows/TaskScheduler -ClassName MSFT_TaskSessionStateChangeTrigger
    $unlock = New-CimInstance -CimClass $class -ClientOnly -Property @{StateChange=8;UserId=$user}
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName 'TransparencyTweak' -Action $action -Trigger @($logon,$unlock) -Principal $principal -Settings $settings -Force -Description ('Transparency Tweak Aero ' + $package.Version) | Out-Null
}

function Restart-TweakHosts {
    # These operations run only when the user runs the installer/restore tool.
    @(Get-Process SystemSettings -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session |
        Where-Object { @($_.Modules | Where-Object ModuleName -match '^TweakTap(?:-.*)?\.dll$').Count -gt 0 }) | Stop-Process -Force
    @(Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session) | Stop-Process -Force
    Start-Sleep -Seconds 2
    if (-not @(Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session).Count) {
        Start-Process -FilePath (Join-Path $env:WINDIR 'explorer.exe') -WindowStyle Hidden
    }
}

function Load-Executable([string]$path, [string]$expectedHash) {
    if (-not $path) { return }
    $process = Start-Process -FilePath $path -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(60000)) { $process.Kill(); throw 'Injector timed out.' }
    if ($process.ExitCode -ne 0) { throw ('Injector failed: ' + $process.ExitCode) }
    $found = @(Get-Process explorer -ErrorAction SilentlyContinue | Where-Object SessionId -eq $session | ForEach-Object {
        @($_.Modules | Where-Object ModuleName -match '^TweakTap(?:-.*)?\.dll$')
    })
    if ($found.Count -ne 1 -or (Get-FileHash -LiteralPath $found[0].FileName).Hash -ne $expectedHash) {
        throw 'Expected DLL was not loaded as the only tweak in Explorer.'
    }
}

function Restore-Previous($state) {
    Stop-ScheduledTask -TaskName 'TransparencyTweak' -ErrorAction SilentlyContinue
    Restore-Preferences $state
    if ($state.TaskXML) {
        Register-ScheduledTask -TaskName 'TransparencyTweak' -Xml $state.TaskXML -Force | Out-Null
    } else { Unregister-ScheduledTask -TaskName 'TransparencyTweak' -Confirm:$false -ErrorAction SilentlyContinue }
    Restart-TweakHosts
    Load-Executable $state.PreviousExecutable $state.PreviousDLLSHA256
    $state.Active = $false
    $state | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $statePath -Encoding UTF8
}

$state = if (Test-Path -LiteralPath $statePath) { Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json } else { $null }
if ($state -and $state.Version -ne $package.Version) { throw 'Saved state belongs to another version.' }
if ($Mode -eq 'Previous') {
    if (-not $state -or -not $state.Active) { throw 'No active installation with a saved previous version.' }
    if ($currentExe -ne $exe) { throw 'Another version controls startup. This restore tool will not change it.' }
    Assert-PreviousExecutable $state.PreviousExecutable
    if ($state.PreviousExecutable -and (Get-FileHash -LiteralPath (Join-Path (Split-Path $state.PreviousExecutable) 'TweakTap.dll')).Hash -ne $state.PreviousDLLSHA256) {
        throw 'Previous DLL changed after installation. Reinstall that version explicitly instead.'
    }
} else {
    foreach ($file in @('TweakTap.dll','TransparencyTweak.exe')) {
        $source = Join-Path $PSScriptRoot ('native\out\' + $file)
        $hash = $package.Hashes.PSObject.Properties[$file].Value
        if ((Get-FileHash -LiteralPath $source).Hash -ne $hash) { throw "Payload checksum mismatch: $file" }
        $existing = Join-Path $versionDir $file
        if ((Test-Path -LiteralPath $existing) -and (Get-FileHash -LiteralPath $existing).Hash -ne $hash) {
            throw 'This version folder contains different binaries. Build a new version number; nothing was overwritten.'
        }
    }
    if ($currentExe -eq $exe -and (-not $state -or -not $state.Active)) { throw 'Previous state is missing; refusing to overwrite it.' }
    if ($state -and $state.Active -and $currentExe -ne $exe) { throw 'Another version is active. Restore or reinstall that version first.' }
    if (-not $state -or -not $state.Active) {
        $previousExe = if ($currentExe) { $currentExe } else {
            $legacyExe = Join-Path $root 'app\TransparencyTweak.exe'
            if (Test-Path -LiteralPath $legacyExe) { $legacyExe } else { $null }
        }
        Assert-PreviousExecutable $previousExe
        $state = [pscustomobject]@{
            Version=$package.Version;Active=$false;CreatedUTC=[DateTime]::UtcNow.ToString('o')
            PreviousExecutable=$previousExe
            PreviousDLLSHA256=if($previousExe){(Get-FileHash -LiteralPath (Join-Path (Split-Path $previousExe) 'TweakTap.dll')).Hash}else{$null}
            TaskXML=if($task){Export-ScheduledTask -TaskName 'TransparencyTweak'}else{$null}
            Preferences=@(Read-Preferences)
        }
    }
}
if ($Plan) {
    [pscustomobject]@{Mode=$Mode;Version=$package.Version;TargetDirectory=$versionDir;PreviousExecutable=$state.PreviousExecutable;ExistingProgramFilesPreserved=$true;ExplorerRestartRequired=$true}
    return
}

if ($Mode -eq 'Previous') {
    Restore-Previous $state
    Write-Host 'Previous version and saved preferences restored. Aero package files are kept.'
    return
}
New-Item -ItemType Directory -Path $versionDir -Force | Out-Null
$state | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $statePath -Encoding UTF8
try {
    foreach ($file in @('TweakTap.dll','TransparencyTweak.exe')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('native\out\' + $file)) -Destination (Join-Path $versionDir $file) -Force
    }
    (Get-Item -LiteralPath (Join-Path $versionDir 'TweakTap.dll')).LastWriteTimeUtc = [DateTime]::Parse($package.DLLLastWriteTimeUTC).ToUniversalTime()
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'package.json') -Destination (Join-Path $versionDir 'package.json') -Force
    if (-not (Test-Path -LiteralPath $key)) { New-Item -Path $key -Force | Out-Null }
    if (-not (Test-Path -LiteralPath $personalize)) { New-Item -Path $personalize -Force | Out-Null }
    if ($null -eq (Get-ItemProperty -LiteralPath $key).TaskbarTransparency) { Set-ItemProperty -LiteralPath $key -Name TaskbarTransparency -Value 100 -Type DWord }
    Set-ItemProperty -LiteralPath $key -Name AeroGlass -Value 1 -Type DWord
    Set-ItemProperty -LiteralPath $personalize -Name EnableTransparency -Value 1 -Type DWord
    Stop-ScheduledTask -TaskName 'TransparencyTweak' -ErrorAction SilentlyContinue
    Register-AeroTask
    Restart-TweakHosts
    Load-Executable $exe $package.Hashes.'TweakTap.dll'
    $state.Active = $true
    $state | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $statePath -Encoding UTF8
    Write-Host ('Transparency Tweak Aero ' + $package.Version + ' installed in ' + $versionDir)
    Write-Host 'Previous program files are preserved. Open Personalization > Colors for Aero Glass, Blur strength and Reflection strength.'
} catch {
    $failure = $_.Exception.Message
    try { Restore-Previous $state } catch { throw "Install failed: $failure. Automatic restore also failed: $($_.Exception.Message). Use the previous installer." }
    throw "Install failed: $failure. Previous version and preferences restored."
}
