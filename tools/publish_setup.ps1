# Publishes the signed setup to the Miniserver, behind a free account (pb_hooks/chronodrive.pb.js there):
#   pb_private/chronodrive/ChronoDrive_Setup.exe     served by GET /api/cdrive/download to signed-in users
#   pb_public/chronodrive/windows.json               {version, size, sha256, notes, published}, public
# The version is the installer's (AppVersion in installer\ChronoDrive.iss). The page is /chronodrive/.
#
#   pwsh -File tools\publish_setup.ps1 -Notes "What is new, one line"
#
# Build and sign first: pwsh -File tools\build_setup.ps1
param([string]$Notes = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$iss = Get-Content (Join-Path $root "installer\ChronoDrive.iss") -Raw
if ($iss -notmatch '#define AppVersion "([^"]+)"') { throw "AppVersion not found in ChronoDrive.iss" }
$ver = $Matches[1]
$setup = Join-Path $root "installer\Output\ChronoDrive_Setup_$ver.exe"
if (-not (Test-Path $setup)) { throw "no setup at $setup" }
$sig = Get-AuthenticodeSignature $setup
if ($sig.Status -ne 'Valid') { throw "the setup is not signed ($($sig.Status)): build it with tools\build_setup.ps1" }
$hash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
$size = (Get-Item $setup).Length
$ms = "I:\LLMLab\Azure\miniserver"
$priv = Join-Path $ms "pb_private\chronodrive"; $pub = Join-Path $ms "pb_public\chronodrive"
New-Item -ItemType Directory -Force $priv, $pub | Out-Null
Copy-Item $setup (Join-Path $priv "ChronoDrive_Setup.exe") -Force
$json = @{ version = $ver; size = $size; sha256 = $hash; notes = $Notes; published = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ") } | ConvertTo-Json -Compress
[IO.File]::WriteAllText((Join-Path $pub "windows.json"), $json)
$K = "$env:USERPROFILE\.ssh\azure_miniserver"; $H = "jose@cs-miniserver.swedencentral.cloudapp.azure.com"
& ssh -i $K $H "mkdir -p /opt/miniserver/pb_private/chronodrive /opt/miniserver/pb_public/chronodrive"
& scp -i $K (Join-Path $priv "ChronoDrive_Setup.exe") "${H}:/opt/miniserver/pb_private/chronodrive/"
if ($LASTEXITCODE) { throw "scp failed ($LASTEXITCODE)" }
& scp -i $K (Join-Path $pub "windows.json") "${H}:/opt/miniserver/pb_public/chronodrive/"
if ($LASTEXITCODE) { throw "scp failed ($LASTEXITCODE)" }
$live = Invoke-RestMethod "https://cs-miniserver.chronoscan.org/api/cdrive/status"
Write-Host "published ChronoDrive $ver ($([math]::Round($size/1MB,1)) MB, sha256 $($hash.Substring(0,12))...) - /api/cdrive/status says $($live.version), online=$($live.online)"
