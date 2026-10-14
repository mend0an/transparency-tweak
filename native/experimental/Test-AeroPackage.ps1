# Artifact checks and simulated installer round trips. Never touches real tasks,
# registry or processes. All filesystem fixtures stay under native/out.
[CmdletBinding()]
param([string]$Version = '0.1.0-preview.2')
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
$global:AeroPackageTest = @{Task=$null;Registry=@{};Module=$null;FailAero=$false;Writes=0;TaskXML=$null;TaskbarReads=0;DelayReads=0;ExtraExplorer=$false;WrongAeroModule=$null;DuplicateAeroModule=$null}
try {
    # Simulate taskbar ownership without reading the real desktop.
    Add-Type -TypeDefinition @'
using System;
namespace TransparencyTweak {
    public static class AeroInstallerNative {
        public static IntPtr FindWindow(string className, string windowName) { return new IntPtr(1); }
        public static uint GetWindowThreadProcessId(IntPtr window, out uint processId) { processId = 100; return 1; }
    }
}
'@
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
        if($Id -eq $PID){return [pscustomobject]@{SessionId=0}}
        if($Name -eq 'explorer' -or $Id -eq 100){
            if($Id -eq 100){$global:AeroPackageTest.TaskbarReads++}
            $modules = @()
            if($global:AeroPackageTest.Module -and ($Id -ne 100 -or $global:AeroPackageTest.TaskbarReads -gt $global:AeroPackageTest.DelayReads)){
                $modules += [pscustomobject]@{ModuleName='TweakTap-test.dll';FileName=$global:AeroPackageTest.Module}
                if($global:AeroPackageTest.DuplicateAeroModule -and $global:AeroPackageTest.Module -like '*\versions\*'){
                    $modules += [pscustomobject]@{ModuleName='TweakTap-old.dll';FileName=$global:AeroPackageTest.DuplicateAeroModule}
                }
            }
            [pscustomobject]@{Id=100;SessionId=0;Modules=$modules;ProcessName='explorer'}
            if(-not $Id -and $global:AeroPackageTest.ExtraExplorer){
                [pscustomobject]@{Id=101;SessionId=0;Modules=@([pscustomobject]@{ModuleName='TweakTap-old.dll';FileName=$global:AeroPackageTest.LegacyModule});ProcessName='explorer'}
            }
        }
    }
    function Stop-Process { param([Parameter(ValueFromPipeline=$true)]$InputObject,[switch]$Force) process{$global:AeroPackageTest.Module=$null} }
    function Start-Sleep { param($Seconds,$Milliseconds) }
    function Start-Process {
        param($FilePath,$WindowStyle,[switch]$PassThru)
        Assert ($WindowStyle -eq 'Hidden') 'Installer exposed an interactive helper window.'
        if($FilePath -like '*TransparencyTweak.exe'){
            $failed = $global:AeroPackageTest.FailAero -and $FilePath -like '*\versions\*'
            $global:AeroPackageTest.TaskbarReads=0
            if(-not $failed){
                $global:AeroPackageTest.Module = if($global:AeroPackageTest.WrongAeroModule -and $FilePath -like '*\versions\*'){$global:AeroPackageTest.WrongAeroModule}else{Join-Path (Split-Path $FilePath) 'TweakTap.dll'}
            }
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
    $global:AeroPackageTest.LegacyModule=Join-Path $legacyDir 'TweakTap.dll'
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
    $global:AeroPackageTest.DelayReads=3
    $global:AeroPackageTest.ExtraExplorer=$true
    & $switch -Mode Enable
    Assert ($global:AeroPackageTest.TaskbarReads -eq 4) 'Installer did not wait for delayed taskbar DLL loading.'
    & $switch -Mode Previous
    $global:AeroPackageTest.DelayReads=0
    $global:AeroPackageTest.ExtraExplorer=$false
    Write-Host 'PASS: delayed DLL readiness and separate Explorer processes do not cause false failures.'
    foreach($scenario in @('WrongAeroModule','DuplicateAeroModule')){
        $global:AeroPackageTest[$scenario]=$global:AeroPackageTest.LegacyModule
        $rejected=$false
        try { & $switch -Mode Enable } catch {
            $rejected=$_.Exception.Message -like '*Expected SHA256*Previous version and preferences restored*'
        }
        Assert $rejected ('Invalid taskbar DLL accepted or rollback failed: ' + $scenario)
        Assert ($global:AeroPackageTest.TaskXML -eq $initialXML) 'Verification rollback changed prior startup task.'
        Assert ($global:AeroPackageTest.Registry[$key].AeroGlass -eq 0) 'Verification rollback changed prior preference.'
        $global:AeroPackageTest[$scenario]=$null
    }
    $diagnostics=Get-Content -LiteralPath (Join-Path $fixture 'TransparencyTweak\logs\aero-install.log') -Raw
    Assert ($diagnostics -like '*Taskbar Explorer PID 100*' -and $diagnostics.Contains($legacyHash)) 'Diagnostic log omitted observed PID/hash.'
    Write-Host 'PASS: wrong/duplicate taskbar DLLs are rejected, logged and rolled back.'
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
