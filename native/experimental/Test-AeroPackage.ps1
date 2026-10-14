# Artifact checks and simulated installer round trips. Never touches real tasks,
# registry or processes. All filesystem fixtures stay under native/out.
[CmdletBinding()]
param([string]$Version = '0.1.0-preview.1')
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
$release = Join-Path $root ('dist\TransparencyTweak-Aero-' + $Version)
function Assert($condition, [string]$message) { if (-not $condition) { throw $message } }
$package = Get-Content -LiteralPath (Join-Path $release 'package.json') -Raw | ConvertFrom-Json
foreach ($name in @(('Install Transparency Tweak Aero ' + $Version + '.exe'),'Restore Previous Version.exe')) {
    $assembly = [Reflection.Assembly]::ReflectionOnlyLoadFrom((Join-Path $release $name))
    foreach ($resource in $assembly.GetManifestResourceNames()) {
        $stream = $assembly.GetManifestResourceStream($resource)
        try {
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $actual = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','') } finally { $sha.Dispose() }
            Assert ($actual -eq (Get-FileHash -LiteralPath (Join-Path $release $resource)).Hash) "Embedded resource differs: $name / $resource"
        } finally { $stream.Dispose() }
    }
}
Write-Host 'PASS: standalone EXE resources match scripts, identity and native payloads.'
foreach ($line in Get-Content -LiteralPath (Join-Path $release 'SHA256SUMS.txt')) {
    Assert ($line -match '^([a-f0-9]{64})  (.+)$') 'Malformed checksum entry.'
    Assert ((Get-FileHash -LiteralPath (Join-Path $release $Matches[2])).Hash -eq $Matches[1]) ('Checksum mismatch: ' + $Matches[2])
}
Write-Host 'PASS: release checksums.'

$fixture = Join-Path $root ('native\out\aero-prototype\package-test-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
$oldLocal = $env:LOCALAPPDATA
$env:LOCALAPPDATA = $fixture
$global:AeroPackageTest = @{Task=$null;Registry=@{};Module=$null;FailAero=$false;Writes=0;TaskXML=$null}
try {
    # Functions are inherited by the child script. Native system mutations are
    # replaced, while versioned file copy/state persistence uses real fixtures.
    function Get-ScheduledTask { param($TaskName,$ErrorAction) if($global:AeroPackageTest.Task){$global:AeroPackageTest.Task} }
    function Export-ScheduledTask { param($TaskName) $global:AeroPackageTest.TaskXML }
    function Stop-ScheduledTask { param($TaskName,$ErrorAction) }
    function Unregister-ScheduledTask { param($TaskName,$Confirm,$ErrorAction) $global:AeroPackageTest.Task=$null; $global:AeroPackageTest.Writes++ }
    function New-ScheduledTaskAction { param($Execute) [pscustomobject]@{Execute=$Execute;Arguments=''} }
    function New-ScheduledTaskTrigger { param([switch]$AtLogOn,$User) [pscustomobject]@{User=$User} }
    function New-ScheduledTaskPrincipal { param($UserId,$LogonType,$RunLevel) [pscustomobject]@{User=$UserId} }
    function New-ScheduledTaskSettingsSet { param([switch]$AllowStartIfOnBatteries,[switch]$DontStopIfGoingOnBatteries,$ExecutionTimeLimit,$MultipleInstances) [pscustomobject]@{} }
    function Get-CimClass { param($Namespace,$ClassName) [pscustomobject]@{} }
    function New-CimInstance { param($CimClass,[switch]$ClientOnly,$Property) [pscustomobject]@{} }
    function Register-ScheduledTask {
        param($TaskName,$Action,$Trigger,$Principal,$Settings,[switch]$Force,$Description,$Xml)
        if ($Xml) { $command = ([xml]$Xml).Task.Actions.Exec.Command; $Action = [pscustomobject]@{Execute=[string]$command;Arguments=''} }
        $global:AeroPackageTest.Task=[pscustomobject]@{Actions=@($Action)}
        $global:AeroPackageTest.TaskXML = if($Xml){$Xml}else{'<Task><Actions><Exec><Command>' + [Security.SecurityElement]::Escape($Action.Execute) + '</Command></Exec></Actions></Task>'}
        $global:AeroPackageTest.Writes++
    }
    function Get-ItemProperty {
        param($LiteralPath,$ErrorAction)
        if($global:AeroPackageTest.Registry.ContainsKey($LiteralPath)){[pscustomobject]$global:AeroPackageTest.Registry[$LiteralPath]}
    }
    function Set-ItemProperty {
        param($LiteralPath,$Name,$Value,$Type)
        if(-not $global:AeroPackageTest.Registry.ContainsKey($LiteralPath)){$global:AeroPackageTest.Registry[$LiteralPath]=@{}}
        $global:AeroPackageTest.Registry[$LiteralPath][$Name]=$Value; $global:AeroPackageTest.Writes++
    }
    function Remove-ItemProperty {
        param($LiteralPath,$Name,$ErrorAction)
        if($global:AeroPackageTest.Registry.ContainsKey($LiteralPath)){$global:AeroPackageTest.Registry[$LiteralPath].Remove($Name)}
        $global:AeroPackageTest.Writes++
    }
    function Test-Path {
        param($LiteralPath,$PathType='Any')
        if($LiteralPath -like 'HKCU:*'){return $global:AeroPackageTest.Registry.ContainsKey($LiteralPath)}
        Microsoft.PowerShell.Management\Test-Path -LiteralPath $LiteralPath -PathType $PathType
    }
    function New-Item {
        param($Path,$ItemType,[switch]$Force)
        if($Path -like 'HKCU:*'){$global:AeroPackageTest.Registry[$Path]=@{}; $global:AeroPackageTest.Writes++; return}
        Microsoft.PowerShell.Management\New-Item -Path $Path -ItemType $ItemType -Force:$Force
    }
    function Get-Process {
        param($Name,$Id,$ErrorAction)
        if($Id){return [pscustomobject]@{SessionId=0}}
        if($Name -eq 'explorer'){
            $modules = if($global:AeroPackageTest.Module){@([pscustomobject]@{ModuleName='TweakTap-test.dll';FileName=$global:AeroPackageTest.Module})}else{@()}
            [pscustomobject]@{SessionId=0;Modules=$modules;ProcessName='explorer'}
        }
    }
    function Stop-Process { param([Parameter(ValueFromPipeline=$true)]$InputObject,[switch]$Force) process{$global:AeroPackageTest.Module=$null} }
    function Start-Sleep { param($Seconds) }
    function Start-Process {
        param($FilePath,$WindowStyle,[switch]$PassThru)
        Assert ($WindowStyle -eq 'Hidden') 'Installer exposed an interactive helper window.'
        if($FilePath -like '*TransparencyTweak.exe'){
            $failed = $global:AeroPackageTest.FailAero -and $FilePath -like '*\versions\*'
            if(-not $failed){$global:AeroPackageTest.Module = Join-Path (Split-Path $FilePath) 'TweakTap.dll'}
            $process=[pscustomobject]@{ExitCode=if($failed){1}else{0}}
            $process | Add-Member ScriptMethod WaitForExit {param($timeout) return $true}
            $process | Add-Member ScriptMethod Kill {throw 'Unexpected timeout.'}
            return $process
        }
    }
    $key='HKCU:\Software\TransparencyTweak'
    $personalize='HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
    $legacyDir=Join-Path $fixture 'TransparencyTweak\app'
    [IO.Directory]::CreateDirectory($legacyDir) | Out-Null
    foreach($file in @('TweakTap.dll','TransparencyTweak.exe')){[IO.File]::WriteAllText((Join-Path $legacyDir $file),'previous binary ' + $file)}
    $previousExe=Join-Path $legacyDir 'TransparencyTweak.exe'
    $legacyHash=(Get-FileHash -LiteralPath (Join-Path $legacyDir 'TweakTap.dll')).Hash
    $initialXML='<Task><Actions><Exec><Command>' + [Security.SecurityElement]::Escape($previousExe) + '</Command></Exec></Actions></Task>'
    $global:AeroPackageTest.Task=[pscustomobject]@{Actions=@([pscustomobject]@{Execute=$previousExe;Arguments=''})}
    $global:AeroPackageTest.TaskXML=$initialXML
    $global:AeroPackageTest.Registry[$key]=@{TaskbarTransparency=100;AeroGlass=0;GlassReflectionPercent=9}
    $global:AeroPackageTest.Registry[$personalize]=@{EnableTransparency=0}
    $switch=Join-Path $release 'switch.ps1'
    $plan=& $switch -Mode Enable -Plan
    Assert ($global:AeroPackageTest.Writes -eq 0) 'Plan mutated system state.'
    Assert ($plan.PreviousExecutable -eq $previousExe) 'Plan lost previous version.'
    & $switch -Mode Enable
    Assert ($global:AeroPackageTest.Task.Actions[0].Execute -eq (Join-Path $plan.TargetDirectory 'TransparencyTweak.exe')) 'Startup action did not switch.'
    Assert ($global:AeroPackageTest.Registry[$key].TaskbarTransparency -eq 100) 'Transparency changed.'
    Assert ($global:AeroPackageTest.Registry[$key].AeroGlass -eq 1) 'Glass not enabled.'
    Assert ((Get-FileHash -LiteralPath (Join-Path $legacyDir 'TweakTap.dll')).Hash -eq $legacyHash) 'Previous DLL overwritten.'
    & $switch -Mode Previous
    Assert ($global:AeroPackageTest.TaskXML -eq $initialXML) 'Previous startup task not restored.'
    Assert ($global:AeroPackageTest.Registry[$key].AeroGlass -eq 0) 'Previous glass preference not restored.'
    Assert ($global:AeroPackageTest.Registry[$key].GlassReflectionPercent -eq 9) 'Reflection preference not restored.'
    Assert ($global:AeroPackageTest.Registry[$personalize].EnableTransparency -eq 0) 'Windows preference not restored.'
    Assert (Test-Path -LiteralPath (Join-Path $plan.TargetDirectory 'TweakTap.dll')) 'Restore deleted Aero files.'
    Write-Host 'PASS: install/restore keeps both versions, slider and previous startup/preferences.'
    $global:AeroPackageTest.FailAero=$true
    $rejected=$false
    try { & $switch -Mode Enable } catch { $rejected=$_.Exception.Message -like '*Previous version and preferences restored*' }
    Assert $rejected 'Failed activation did not report successful rollback.'
    Assert ($global:AeroPackageTest.TaskXML -eq $initialXML) 'Rollback changed prior startup task.'
    Assert ($global:AeroPackageTest.Registry[$key].AeroGlass -eq 0) 'Rollback changed prior preference.'
    Write-Host 'PASS: injection failure rolls back.'
    $global:AeroPackageTest.FailAero=$false
    & $switch -Mode Enable
    $global:AeroPackageTest.Task.Actions[0].Execute = Join-Path $fixture 'TransparencyTweak\versions\other\TransparencyTweak.exe'
    $writes=$global:AeroPackageTest.Writes
    $rejected=$false
    try { & $switch -Mode Previous } catch { $rejected=$_.Exception.Message -like '*Another version controls startup*' }
    Assert ($rejected -and $writes -eq $global:AeroPackageTest.Writes) 'Restore overwrote another version.'
    Write-Host 'PASS: restore refuses to disturb another active version.'
    $global:AeroPackageTest.Task.Actions[0].Execute = Join-Path $plan.TargetDirectory 'TransparencyTweak.exe'
    [IO.File]::WriteAllText((Join-Path $legacyDir 'TweakTap.dll'),'changed previous version')
    $writes=$global:AeroPackageTest.Writes
    $rejected=$false
    try { & $switch -Mode Previous } catch { $rejected=$_.Exception.Message -like '*Previous DLL changed*' }
    Assert ($rejected -and $writes -eq $global:AeroPackageTest.Writes) 'Restore activated a changed previous DLL.'
    Write-Host 'PASS: changed previous DLL is rejected before mutations.'
    [IO.File]::WriteAllText((Join-Path $plan.TargetDirectory 'TweakTap.dll'),'different same-version build')
    $writes=$global:AeroPackageTest.Writes
    $rejected=$false
    try { & $switch -Mode Enable } catch { $rejected=$_.Exception.Message -like '*different binaries*' }
    Assert ($rejected -and $writes -eq $global:AeroPackageTest.Writes) 'Install overwrote a conflicting version folder.'
    Write-Host 'PASS: conflicting version binaries are preserved and rejected.'
    $env:LOCALAPPDATA=Join-Path $fixture 'fresh-user'
    $global:AeroPackageTest.Task=$null
    $global:AeroPackageTest.TaskXML=$null
    $global:AeroPackageTest.Registry=@{}
    $global:AeroPackageTest.Module=$null
    & $switch -Mode Enable
    Assert ($global:AeroPackageTest.Registry[$key].TaskbarTransparency -eq 100) 'Fresh install default missing.'
    & $switch -Mode Previous
    Assert ($null -eq $global:AeroPackageTest.Task -and $null -eq $global:AeroPackageTest.Module) 'Fresh restore left an active tweak.'
    Assert (-not $global:AeroPackageTest.Registry[$personalize].ContainsKey('EnableTransparency')) 'Fresh restore changed prior Windows preference.'
    Write-Host 'PASS: fresh install restores Windows without leaving a startup task.'
} finally {
    $env:LOCALAPPDATA=$oldLocal
    Remove-Variable AeroPackageTest -Scope Global -ErrorAction SilentlyContinue
}
Write-Host 'LIMIT: simulated Windows APIs; no live install, restart or visual Settings test.'
