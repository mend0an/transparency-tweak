# Builds only the isolated probe. Stable DLL, injector and installers are untouched.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ x64 Build Tools are required.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$out = Join-Path (Split-Path $PSScriptRoot) 'out\aero-prototype'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$source = Join-Path $PSScriptRoot 'AeroGlassProbe.cpp'
$object = Join-Path $out 'AeroGlassProbe.obj'
$exe = Join-Path $out 'AeroGlassProbe.exe'
$buildCommand = 'call "{0}" >nul && cl /nologo /std:c++20 /EHsc /O2 /MT /W4 /permissive- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fo:"{1}" /Fe:"{2}" "{3}" /link windowsapp.lib ole32.lib user32.lib psapi.lib CoreMessaging.lib dxguid.lib' -f $vcvars,$object,$exe,$source
& $env:ComSpec /d /s /c $buildCommand
if ($LASTEXITCODE -ne 0) { throw 'Experimental probe build failed.' }
Write-Host "Built $exe"

$native = Split-Path $PSScriptRoot
$tap = Join-Path $native 'TweakTap.cpp'
$injector = Join-Path $native 'Injector.cpp'
foreach ($variant in @(
    @{ Source=(Join-Path $PSScriptRoot 'TrayShadowProbe.cpp'); Output='TrayShadowProbe.exe'; Object='TrayShadowProbe.obj'; Flags=''; Libraries=('windowsapp.lib ole32.lib user32.lib /MANIFEST:EMBED /MANIFESTINPUT:"{0}"' -f (Join-Path $PSScriptRoot 'TrayShadowProbe.manifest')) },
    @{ Source=$tap; Output='TweakTap.dll'; Object='TweakTap-aero.obj'; Flags='/LD /DTWEAK_EXPERIMENTAL_AERO'; Libraries='windowsapp.lib ole32.lib oleaut32.lib advapi32.lib user32.lib dxguid.lib' },
    @{ Source=$tap; Output='TweakTap-stable-check.dll'; Object='TweakTap-stable.obj'; Flags='/LD'; Libraries='windowsapp.lib ole32.lib oleaut32.lib advapi32.lib user32.lib' },
    @{ Source=$injector; Output='TransparencyTweak.exe'; Object='Injector.obj'; Flags=''; Libraries='/SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup user32.lib advapi32.lib' }
)) {
    $variantObject = Join-Path $out $variant.Object
    $variantOutput = Join-Path $out $variant.Output
    $command = 'call "{0}" >nul && cl /nologo /std:c++20 /EHsc /O2 /MT /W4 /permissive- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX {1} /Fo:"{2}" /Fe:"{3}" "{4}" /link {5}' -f $vcvars,$variant.Flags,$variantObject,$variantOutput,$variant.Source,$variant.Libraries
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw ('Experimental build failed: ' + $variant.Output) }
    Write-Host "Built $variantOutput"
}
