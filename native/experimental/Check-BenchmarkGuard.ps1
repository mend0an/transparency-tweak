# Read-only prerequisite check: on a stable installation the benchmark must
# stop before changing any registry values.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$key = 'HKCU:\Software\TransparencyTweak'
$before = Get-ItemProperty -LiteralPath $key | Select-Object TaskbarTransparency,AeroGlass | ConvertTo-Json -Compress
$session = (Get-Process -Id $PID).SessionId
$explorer = Get-Process explorer | Where-Object SessionId -eq $session | Select-Object -First 1
$candidate = Join-Path $PSScriptRoot '..\out\aero-prototype\TweakTap.dll'
$candidateHash = (Get-FileHash -LiteralPath $candidate).Hash
$activeCandidate = @($explorer.Modules | Where-Object ModuleName -like 'TweakTap*' | Where-Object {
    (Get-FileHash -LiteralPath $_.FileName).Hash -eq $candidateHash
})
if ($activeCandidate.Count) { throw 'This guard test requires the stable DLL to be active.' }
$rejected = $false
try {
    & (Join-Path $PSScriptRoot 'Measure-AeroTaskbar.ps1')
} catch {
    if ($_.Exception.Message -notmatch 'experimental DLL is not loaded') { throw }
    $rejected = $true
} finally {
    $after = Get-ItemProperty -LiteralPath $key | Select-Object TaskbarTransparency,AeroGlass | ConvertTo-Json -Compress
    if ($after -ne $before) { throw 'Benchmark guard unexpectedly changed settings.' }
}
if (-not $rejected) { throw 'Benchmark did not reject the stable DLL.' }
Write-Output 'PASS: benchmark refused the stable DLL; registry settings are unchanged.'
