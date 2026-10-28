# Configure + build ChronoDrive (Release by default).
#   .\build.ps1                -> build\Release\{ChronoDrive.exe, cdrive.exe}
#   .\build.ps1 -Config Debug
#   .\build.ps1 -Run           -> build, then launch the app
param([string]$Config = "Release", [switch]$Run, [string]$BuildDir = "build", [string]$Target = "")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
cmake -S $root -B "$root\$BuildDir" -A x64 | Out-Host
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$targs = @()
if ($Target) { $targs = @("--target", $Target) }
cmake --build "$root\$BuildDir" --config $Config @targs -- /m /v:minimal | Out-Host
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Write-Host "built: $root\$BuildDir\$Config"
if ($Run) { Start-Process "$root\$BuildDir\$Config\ChronoDrive.exe" }
