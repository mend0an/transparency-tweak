# Builds a separately named, versioned Aero release; stable outputs stay intact.
[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+-preview\.\d+$')][string]$Version = '0.1.0-preview.5',
    [switch]$SkipNativeBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
$nativeOut = Join-Path $root 'native\out\aero-prototype'
$release = Join-Path $root ('dist\TransparencyTweak-Aero-' + $Version)
if (Test-Path -LiteralPath $release) { throw 'This release already exists. Use a new version number; no release files were overwritten.' }
$protected = @('dist\Install Transparency Tweak.exe','dist\Uninstall Transparency Tweak.exe','native\out\TweakTap.dll','native\out\TransparencyTweak.exe')
$originalHashes = @{}
foreach ($relative in $protected) {
    $path = Join-Path $root $relative
    if (Test-Path -LiteralPath $path) { $originalHashes[$relative] = (Get-FileHash -LiteralPath $path).Hash }
}
if (-not $SkipNativeBuild) { & (Join-Path $PSScriptRoot 'build-aero-prototype.ps1') }
foreach ($file in @('TweakTap.dll','TransparencyTweak.exe','AeroGlassProbe.exe','TrayShadowProbe.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $nativeOut $file))) { throw "Required build output missing: $file" }
}
& (Join-Path $nativeOut 'AeroGlassProbe.exe')
if ($LASTEXITCODE -ne 0) { throw 'Aero graph probe failed. Release not created.' }
& (Join-Path $nativeOut 'TrayShadowProbe.exe')
if ($LASTEXITCODE -ne 0) { throw 'Tray shadow probe failed. Release not created.' }
New-Item -ItemType Directory -Path (Join-Path $release 'native\out') -Force | Out-Null
foreach ($file in @('TweakTap.dll','TransparencyTweak.exe')) {
    Copy-Item -LiteralPath (Join-Path $nativeOut $file) -Destination (Join-Path $release ('native\out\' + $file))
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Install-AeroPackage.ps1') -Destination (Join-Path $release 'install.ps1')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Restore-AeroPackage.ps1') -Destination (Join-Path $release 'uninstall.ps1')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Switch-AeroPackage.ps1') -Destination (Join-Path $release 'switch.ps1')
$hashes = @{}
foreach ($file in @('TweakTap.dll','TransparencyTweak.exe')) { $hashes[$file] = (Get-FileHash -LiteralPath (Join-Path $release ('native\out\' + $file))).Hash }
[pscustomobject]@{
    Product='Transparency Tweak Aero';Channel='aero';Version=$Version;CreatedUTC=[DateTime]::UtcNow.ToString('o')
    WindowsBuildTested='26300.9457';Hashes=$hashes
    DLLLastWriteTimeUTC=(Get-Item -LiteralPath (Join-Path $nativeOut 'TweakTap.dll')).LastWriteTimeUtc.ToString('o')
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $release 'package.json') -Encoding UTF8
$info = Join-Path $nativeOut 'AeroPackageInfo.cs'
('static class AeroPackageInfo { public const string Version = "' + $Version + '"; }') | Set-Content -LiteralPath $info -Encoding UTF8
$manifest = Join-Path $nativeOut 'aero-package.manifest'
(Get-Content -LiteralPath (Join-Path $root 'setup\app.manifest') -Raw).Replace('TransparencyTweak.Setup','TransparencyTweak.Aero.Setup') | Set-Content -LiteralPath $manifest -Encoding UTF8
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$common = @('/nologo','/target:winexe','/optimize+',"/win32manifest:$manifest",'/r:System.dll','/r:System.Drawing.dll','/r:System.Windows.Forms.dll')
$installName = 'Install Transparency Tweak Aero ' + $Version + '.exe'
$restoreName = 'Restore Previous Version.exe'
& $csc @common /define:INSTALL,AERO_PACKAGE "/out:$release\$installName" `
    "/resource:$release\install.ps1,install.ps1" "/resource:$release\switch.ps1,switch.ps1" "/resource:$release\package.json,package.json" `
    "/resource:$release\native\out\TweakTap.dll,native/out/TweakTap.dll" "/resource:$release\native\out\TransparencyTweak.exe,native/out/TransparencyTweak.exe" `
    "$root\setup\Setup.cs" $info
if ($LASTEXITCODE -ne 0) { throw 'Aero installer compilation failed.' }
& $csc @common /define:AERO_PACKAGE "/out:$release\$restoreName" `
    "/resource:$release\uninstall.ps1,uninstall.ps1" "/resource:$release\switch.ps1,switch.ps1" "/resource:$release\package.json,package.json" `
    "$root\setup\Setup.cs" $info
if ($LASTEXITCODE -ne 0) { throw 'Aero restore tool compilation failed.' }
$readme = @"
# Transparency Tweak Aero $Version

Separate Aero preview for Windows 11 x64; runtime tested on build 26300.9457.
This package preserves previous program files. Only one version runs at a time.
The installer verifies the taskbar's Explorer and waits for DLL readiness.
Verification failures include observed DLL paths/hashes in
%LOCALAPPDATA%\TransparencyTweak\logs\aero-install.log.

1. Run **$installName**. No prior uninstall is required.
2. Explorer restarts; File Explorer and Settings windows using the tweak may close.
3. Reopen Settings > Personalization > Colors > Transparency effects.
4. Use Aero Glass, Blur strength, Reflection strength, and Taskbar transparency.
   At 100% transparency, blur and reflections remain; 0% restores native brushes.

System tray text and image icons have a subtle black, glyph-shaped shadow.
It is removed at 0% transparency or when Windows transparency effects are off.
Preview 5 uses arranged text dimensions and layout-sized shadow hosts, keeping
the clock shadow aligned with its digits in the two-line clock/date layout.

Installed files: %LOCALAPPDATA%\TransparencyTweak\versions\aero-$Version
The original app folder is kept. The existing TransparencyTweak startup task
switches to this version; its previous XML and settings are saved locally.

Run **$restoreName** to return to the saved previous version/settings. Aero
files are retained. If this was a fresh install, restore returns to Windows defaults.
Restore refuses to overwrite another version's startup configuration or a changed
previous DLL. The previous version may itself be an earlier Aero prototype.

Do not run the original uninstaller to switch versions: it removes the entire
TransparencyTweak directory, including version folders. Use it only for full removal.

The standalone EXE files include their scripts/payloads. PowerShell alternatives:

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1

The restore operation switches versions; it does not delete the Aero package.
Preview limitation: exact Windows 7 appearance and total blur/GPU allocation are
not established. Preview 5 was installed in a live Windows session; the user
confirmed the corrected tray shadows and supplied taskbar/Settings screenshots.
Automated pixel verification and compatibility with other Windows builds remain
unverified. Building the package does not activate it.
"@
$readme | Set-Content -LiteralPath (Join-Path $release 'README.md') -Encoding UTF8
foreach ($relative in $originalHashes.Keys) {
    if ((Get-FileHash -LiteralPath (Join-Path $root $relative)).Hash -ne $originalHashes[$relative]) { throw "Stable output changed: $relative" }
}
$checksums = Get-ChildItem -LiteralPath $release -Recurse -File | Sort-Object FullName | ForEach-Object {
    ((Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant() + '  ' + $_.FullName.Substring($release.Length + 1).Replace('\','/'))
}
$checksums | Set-Content -LiteralPath (Join-Path $release 'SHA256SUMS.txt') -Encoding ASCII
$zip = $release + '.zip'
if (Test-Path -LiteralPath $zip) { throw 'Release archive already exists; nothing was overwritten.' }
Compress-Archive -Path (Join-Path $release '*') -DestinationPath $zip
Write-Host "Built $release"
Write-Host "Archive: $zip"
Write-Host 'Stable build outputs verified unchanged. The installed version was not modified.'
