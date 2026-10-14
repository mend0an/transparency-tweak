# Build the experimental shell animation separately from the stable installer.
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ Build Tools (x64) not found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
New-Item -ItemType Directory -Path (Join-Path $PSScriptRoot 'out') -Force | Out-Null
$compile = 'cl /nologo /std:c++20 /EHsc /O2 /MT /W4 /WX /permissive- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ShellFlyoutAnimation.cpp /Fo:out\ShellFlyoutAnimation.obj /Fe:out\ShellFlyoutAnimation.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup user32.lib dwmapi.lib shell32.lib'
Push-Location $PSScriptRoot
try {
    cmd /c "call `"$vcvars`" >nul && $compile"
    if ($LASTEXITCODE -ne 0) { throw "Animation build failed ($LASTEXITCODE)." }
    $binary = Join-Path $PSScriptRoot 'out\ShellFlyoutAnimation.exe'
    $test = Start-Process -FilePath $binary -ArgumentList '--self-test' -WindowStyle Hidden -Wait -PassThru
    if ($test.ExitCode -ne 0) { throw 'Animation geometry/target isolation self-test failed.' }
} finally { Pop-Location }
Write-Host "Built $binary; geometry and target isolation self-test passed."
