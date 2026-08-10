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

$cliOut = Join-Path $env:TEMP "arduino-cli-obd-build"
Write-Host "Building $SketchDir version $Version ..."
arduino-cli compile --fqbn $Fqbn --output-dir $cliOut (Join-Path $Root $SketchDir) | Out-Host
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

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

$size = (Get-Item $dest).Length
# Compact JSON, UTF-8 without BOM (BOM breaks some parsers / confuses diffs)
$manifestPath = Join-Path $Root "firmware\manifest.json"
$manifestJson = "{`"version`":`"$Version`",`"file`":`"bin/$destName`",`"size`":$size,`"notes`":`"Published $(Get-Date -Format o)`"}"
[System.IO.File]::WriteAllText($manifestPath, $manifestJson, [System.Text.UTF8Encoding]::new($false))

Write-Host "OK: $dest"
Write-Host "Updated firmware\manifest.json -> $manifestJson"
Write-Host "Next: git add firmware; git commit; git push"

# Best-effort: recreate manifest path to reduce sticky raw.githubusercontent CDN
try {
  # already written; purge jsDelivr mirrors if any clients still use them
  Invoke-WebRequest "https://purge.jsdelivr.net/gh/Vannyhas/ESP32-To-OBD-II@main/firmware/manifest.json" -UseBasicParsing | Out-Null
  Invoke-WebRequest "https://purge.jsdelivr.net/gh/Vannyhas/ESP32-To-OBD-II@main/firmware/bin/$destName" -UseBasicParsing | Out-Null
  Write-Host "jsDelivr purge requested"
} catch {
  Write-Host "jsDelivr purge skipped: $($_.Exception.Message)"
}
