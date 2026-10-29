# Signs one file with ChronoScan's code signing certificate through AutoFirmaCS (same flow as ChronoTwain):
# S:\ = C:\AutoFirmaCS on the PC with the DigiCert token; a file left in S:\IN comes back signed in S:\OUT,
# or in S:\ERROR with <name>.error.txt. Inno Setup calls it for the setup and its uninstaller:
#   ISCC /DSIGN "/Sautofirma=pwsh.exe -NoProfile -ExecutionPolicy Bypass -File $q<this>$q $f" ChronoDrive.iss
# Two things learnt on 2026-10-22: a file that is already signed comes back to OUT with its ORIGINAL date (so it
# is skipped here before sending), and AutoFirma ignores *.tmp (Inno's uninstaller is "uninst.e32.tmp"), so the
# copy in IN always gets a signable name.
param([Parameter(Mandatory)][string]$File, [int]$TimeoutSec = 600)
$ErrorActionPreference = "Stop"
$thumb = "996426BC57433E3B1F201324D6072BC437341237"   # CHRONOSCAN CAPTURE S.L. (DigiCert, until 2027-08-20)
if (-not (Test-Path "S:\IN")) { throw "S:\ (AutoFirmaCS) is not available" }
$have = Get-AuthenticodeSignature $File
if ($have.Status -eq 'Valid' -and $have.SignerCertificate.Thumbprint -eq $thumb) { Write-Host "already signed: $File"; exit 0 }
$leaf = Split-Path $File -Leaf
$name = "cdr-" + [guid]::NewGuid().ToString("N").Substring(0, 8) + "-" + ($leaf -replace '\.tmp$', '')
if ($name -notmatch '\.(exe|dll|e32|msix|cat|sys|ocx)$') { $name += ".exe" }
Remove-Item "S:\OUT\$name", "S:\ERROR\$name", "S:\ERROR\$name.error.txt" -Force -ErrorAction SilentlyContinue
Copy-Item $File "S:\IN\$name" -Force
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    $out = Get-Item "S:\OUT\$name" -ErrorAction SilentlyContinue
    if ($out) {
        $sig = Get-AuthenticodeSignature $out.FullName
        if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Thumbprint -ne $thumb) { throw "$leaf came back from AutoFirma without our valid signature ($($sig.Status))" }
        Copy-Item $out.FullName $File -Force
        Remove-Item $out.FullName -Force -ErrorAction SilentlyContinue
        Write-Host "signed: $leaf"
        exit 0
    }
    $err = Get-Item "S:\ERROR\$name.error.txt" -ErrorAction SilentlyContinue
    if ($err) { throw "AutoFirma could not sign $leaf`n$(Get-Content $err.FullName -Raw)" }
}
throw "AutoFirma did not sign $leaf within $TimeoutSec s (is the AutoFirmaCS task running, the token connected?)"
