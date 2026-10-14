[CmdletBinding()]
param([switch]$Integration)
$ErrorActionPreference = 'Stop'
$binary = Join-Path (Split-Path $PSScriptRoot) 'out\ShellFlyoutAnimation.exe'
if (-not (Test-Path -LiteralPath $binary)) { throw 'Run native/build-flyout-animation.ps1 first.' }
$checks = @('--self-test')
if ($Integration) { $checks += '--integration-test' }
foreach ($check in $checks) {
    $stdout = Join-Path ([IO.Path]::GetTempPath()) ('flyout-test-' + [guid]::NewGuid().ToString('N') + '.txt')
    $process = Start-Process -FilePath $binary -ArgumentList $check -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $stdout
    Get-Content -LiteralPath $stdout
    if ($process.ExitCode -eq 3) { throw "$check could not run on this desktop; validation is incomplete." }
    if ($process.ExitCode -ne 0) { throw "$check failed with exit code $($process.ExitCode)." }
}
Write-Host 'PASS: requested animation checks completed. Real shell visual verification remains separate.'
