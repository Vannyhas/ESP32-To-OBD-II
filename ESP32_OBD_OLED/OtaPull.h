#pragma once

#include <Arduino.h>

// Pull firmware from GitHub (manifest.json + .bin) over phone hotspot Wi‑Fi.
namespace OtaPull {

enum class Status : uint8_t {
  Idle = 0,
  ConnectingWifi,
  WifiFailed,
  FetchingManifest,
  ManifestFailed,
  UpToDate,
  Downloading,
  FlashFailed,
  Rebooting,
};

using StatusFn = void (*)(Status st, const char* detail, int progressPct);

// Blocks until done (or failed). Calls onStatus for UI. Returns true if update
// started and device will reboot; false if no update / error (caller resumes BLE).
bool checkAndUpdate(StatusFn onStatus);

const char* firmwareVersion();

}  // namespace OtaPull
