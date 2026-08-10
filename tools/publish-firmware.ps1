# Publish firmware for GitHub pull-OTA
#
# Usage (from repo root, PowerShell):
#   .\tools\publish-firmware.ps1
#   .\tools\publish-firmware.ps1 -Version 1.0.1
#
# Bumps/copies the Arduino build .bin into firmware/bin/ and updates manifest.json.
# Then: git add firmware && git commit && git push

param(
  [string]$Version = "",
  [string]$SketchDir = "ESP32_OBD_OLED",
  [string]$Fqbn = "esp32:esp32:esp32c6:CDCOnBoot=cdc,PartitionScheme=min_spiffs"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $Root

$config = Join-Path $Root "$SketchDir\config.h"
if (-not (Test-Path $config)) { throw "config.h not found: $config" }

if (-not $Version) {
  $m = Select-String -Path $config -Pattern '#define\s+FIRMWARE_VERSION\s+"([^"]+)"' | Select-Object -First 1
  if (-not $m) { throw "FIRMWARE_VERSION not found in config.h" }
  $Version = $m.Matches[0].Groups[1].Value
}

Write-Host "Building $SketchDir version $Version ..."
arduino-cli compile --fqbn $Fqbn (Join-Path $Root $SketchDir)
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

$buildRoot = Join-Path $env:LOCALAPPDATA "Arduino15\build"
# arduino-cli puts .bin next to sketch build dir — locate newest matching bin
$candidates = @()
$candidates += Get-ChildItem -Path (Join-Path $Root $SketchDir) -Filter "*.bin" -Recurse -ErrorAction SilentlyContinue
$cliOut = Join-Path $env:TEMP "arduino-cli-obd-build"
arduino-cli compile --fqbn $Fqbn --output-dir $cliOut (Join-Path $Root $SketchDir) | Out-Host
if ($LASTEXITCODE -ne 0) { throw "compile --output-dir failed" }

$binSrc = Get-ChildItem -Path $cliOut -Filter "*.ino.bin" | Select-Object -First 1
if (-not $binSrc) {
  $binSrc = Get-ChildItem -Path $cliOut -Filter "*.bin" | Select-Object -First 1
}
if (-not $binSrc) { throw "No .bin found in $cliOut" }

$destName = "esp32-obd-$Version.bin"
$destDir = Join-Path $Root "firmware\bin"
New-Item -ItemType Directory -Force -Path $destDir | Out-Null
$dest = Join-Path $destDir $destName
Copy-Item -Force $binSrc.FullName $dest

$manifest = @{
  version = $Version
  file    = "bin/$destName"
  size    = (Get-Item $dest).Length
  notes   = "Published $(Get-Date -Format o)"
} | ConvertTo-Json

Set-Content -Path (Join-Path $Root "firmware\manifest.json") -Value $manifest -Encoding utf8
Write-Host "OK: $dest"
Write-Host "Updated firmware\manifest.json"
Write-Host "Next: git add firmware; git commit; git push"
