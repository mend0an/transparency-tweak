# Builds out\TweakTap.dll and out\TransparencyTweak.exe with the Visual Studio Build Tools (x64).
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio Build Tools with the C++ workload not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$env:PATH += ";$(Split-Path $vswhere)" # vcvars64.bat calls vswhere by name

$src = $PSScriptRoot
$out = Join-Path $src 'out'
New-Item -ItemType Directory -Force $out | Out-Null

$flags = '/nologo /std:c++20 /EHsc /O2 /MT /W4 /permissive- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX'
$dll = "cl $flags /LD TweakTap.cpp /Fo:out\ /Fe:out\TweakTap.dll /link windowsapp.lib ole32.lib oleaut32.lib advapi32.lib user32.lib"
$exe = "cl $flags Injector.cpp /Fo:out\ /Fe:out\TransparencyTweak.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup user32.lib advapi32.lib"

Push-Location $src
try {
    cmd /c "call `"$vcvars`" >nul && $dll && $exe"
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
} finally { Pop-Location }
Write-Host "Built $out\TweakTap.dll and $out\TransparencyTweak.exe"

# Double-click installer/uninstaller (C# 5, compiler that ships with Windows). The installer embeds
# install.ps1 and the two binaries above, so it is a single file that can be copied anywhere.
$root = Split-Path $src
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$common = @('/nologo', '/target:winexe', '/optimize+', "/win32manifest:$root\setup\app.manifest",
            '/r:System.dll', '/r:System.Drawing.dll', '/r:System.Windows.Forms.dll')

& $csc @common /define:INSTALL "/out:$dist\Install Transparency Tweak.exe" `
    "/resource:$root\install.ps1,install.ps1" `
    "/resource:$out\TweakTap.dll,native/out/TweakTap.dll" `
    "/resource:$out\TransparencyTweak.exe,native/out/TransparencyTweak.exe" `
    "$root\setup\Setup.cs"
if ($LASTEXITCODE -ne 0) { throw "Installer build failed ($LASTEXITCODE)" }

& $csc @common "/out:$dist\Uninstall Transparency Tweak.exe" `
    "/resource:$root\uninstall.ps1,uninstall.ps1" `
    "$root\setup\Setup.cs"
if ($LASTEXITCODE -ne 0) { throw "Uninstaller build failed ($LASTEXITCODE)" }
Write-Host "Built $dist\Install Transparency Tweak.exe and $dist\Uninstall Transparency Tweak.exe"
