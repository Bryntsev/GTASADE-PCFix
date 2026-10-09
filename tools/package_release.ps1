param(
  [string]$Version = '0.2.0',
  [string]$Configuration = 'Release',
  [string]$BuildDir = 'build-x64-ninja',
  [string]$OutputDir = 'out\release'
)

# Builds the player-facing release archive. The archive mirrors the game folder, so players extract
# it into "GTA San Andreas - The Definitive Edition". The ReShade add-on goes to Gameface\Binaries\Win64
# (next to ReShade), the core, helper and ini go to Win64\scripts (where ASI loaders look, next to
# Fusion Fix; the add-on finds the core there too). A core .asi directly in Win64 made the game crash
# on start with Ultimate ASI Loader as version.dll + ReShade + Steam overlay.
# Only the files listed below are packaged: no logs, no developer settings.

$ErrorActionPreference = 'Stop'

$name = "GTASADE-PCFix-v$Version"
$stage = Join-Path $OutputDir $name
$win64 = Join-Path $stage 'Gameface\Binaries\Win64'
$scripts = Join-Path $win64 'scripts'
$zip = Join-Path $OutputDir "$name.zip"

$binaries = @(
  "$BuildDir\$Configuration\GTASADE.PCFix.asi",
  "$BuildDir\$Configuration\GTASADE.PCFix.addon64",
  "$BuildDir\$Configuration\GTASADE.PCFix.RawInputHelper.exe"
)
foreach ($file in $binaries) {
  if (-not (Test-Path $file)) {
    throw "Missing $file. Run tools\build.ps1 -Configuration $Configuration first."
  }
}

# The shipped ini must be the player defaults.
$ini = Get-Content 'config\GTASADE.PCFix.ini' -Raw
if ($ini -notmatch '(?ms)^\[Developer\][^\[]*?^Enabled=0\s*$' -or $ini -notmatch '(?m)^MaxFps=120\s*$') {
  throw 'config\GTASADE.PCFix.ini is not set to player defaults (Developer Enabled=0, MaxFps=120).'
}

if (Test-Path $stage) {
  Remove-Item -Recurse -Force $stage
}
if (Test-Path $zip) {
  Remove-Item -Force $zip
}
New-Item -ItemType Directory -Force -Path $scripts | Out-Null

Copy-Item -LiteralPath "$BuildDir\$Configuration\GTASADE.PCFix.addon64" -Destination $win64
Copy-Item -LiteralPath "$BuildDir\$Configuration\GTASADE.PCFix.asi" -Destination $scripts
Copy-Item -LiteralPath "$BuildDir\$Configuration\GTASADE.PCFix.RawInputHelper.exe" -Destination $scripts
Copy-Item -LiteralPath 'config\GTASADE.PCFix.ini' -Destination $scripts
Copy-Item -LiteralPath 'release\INSTALL.txt' -Destination $stage
Copy-Item -LiteralPath 'LICENSE' -Destination (Join-Path $stage 'LICENSE.txt')
Copy-Item -LiteralPath 'THIRD_PARTY_NOTICES.md' -Destination (Join-Path $stage 'THIRD_PARTY_NOTICES.txt')

Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal

Write-Host "Archive: $zip"
Write-Host ''
Write-Host 'Contents:'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path $zip))
try {
  $archive.Entries | ForEach-Object { '{0,10}  {1}' -f $_.Length, $_.FullName } | Write-Host
} finally {
  $archive.Dispose()
}
Write-Host ''
Write-Host 'SHA256:'
$hashed = @((Resolve-Path $zip).Path) + @(Get-ChildItem $win64 -File -Recurse | ForEach-Object { $_.FullName })
Get-FileHash -Algorithm SHA256 -Path $hashed |
  ForEach-Object { '{0}  {1}' -f $_.Hash, (Split-Path $_.Path -Leaf) } | Write-Host
