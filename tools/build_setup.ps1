# Builds Release, signs the two executables, compiles the Inno Setup and signs the setup (AutoFirmaCS).
#   pwsh -File tools\build_setup.ps1            signed (needs S:\ = AutoFirmaCS with the token)
#   pwsh -File tools\build_setup.ps1 -NoSign    unsigned, for a local test
# The setup is built from its own tree (build-setup\) so an exe left running from build\ never blocks it.
param([switch]$NoSign, [string]$BuildDir = "build-setup")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$iscc = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) { throw "Inno Setup 6 not found at $iscc" }
& (Join-Path $root "build.ps1") -BuildDir $BuildDir
if ($LASTEXITCODE) { throw "build failed" }
$sign = Join-Path $root "installer\sign.ps1"
$iss = Join-Path $root "installer\ChronoDrive.iss"
if (-not $NoSign) {
    foreach ($exe in @("ChronoDrive.exe", "cdrive.exe")) {
        pwsh -NoProfile -ExecutionPolicy Bypass -File $sign (Join-Path $root "$BuildDir\Release\$exe")
        if ($LASTEXITCODE) { throw "signing $exe failed" }
    }
    # $q and $f are Inno Setup placeholders (a double quote, the file): no literal quotes, PowerShell would escape them
    & $iscc "/DBUILDDIR=$BuildDir\Release" /DSIGN "/Sautofirma=pwsh.exe -NoProfile -ExecutionPolicy Bypass -File `$q$sign`$q `$f" $iss | Select-String -Pattern "error|warning|Successful|signed" | Out-Host
} else {
    & $iscc "/DBUILDDIR=$BuildDir\Release" $iss | Select-String -Pattern "error|warning|Successful" | Out-Host
}
if ($LASTEXITCODE) { throw "ISCC failed ($LASTEXITCODE)" }
$text = Get-Content $iss -Raw
$null = $text -match '#define AppVersion "([^"]+)"'; $ver = $Matches[1]
$setup = Join-Path $root "installer\Output\ChronoDrive_Setup_$ver.exe"
$sig = Get-AuthenticodeSignature $setup
Write-Host ("setup: {0} ({1:N1} MB, signature {2})" -f $setup, ((Get-Item $setup).Length / 1MB), $sig.Status)
