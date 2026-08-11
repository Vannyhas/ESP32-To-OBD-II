#pragma once

// ========================= HARDWARE =========================
// Waveshare ESP32-C6-LCD-1.47 (ST7789 IPS 172x320, SPI)
#define LCD_DC         15
#define LCD_CS         14
#define LCD_SCK        7
#define LCD_MOSI       6
#define LCD_MISO       5
#define LCD_RST        21
#define LCD_BL         22
#define SD_CS          4    // keep HIGH so SD doesn't steal SPI
#define LCD_ROTATION   1    // 0=portrait 172x320, 1=landscape 320x172
#define SCREEN_WIDTH   320  // after rotation 1
#define SCREEN_HEIGHT  172

// ========================= BLE OBD =========================
// Leave empty "" to connect to the first BLE device whose name
// contains one of the known OBD prefixes (OBD, ELM, Vgate, ...).
// Or put exact name / MAC, e.g. "OBDII" or "AA:BB:CC:DD:EE:FF"
#define OBD_TARGET_NAME  "66:1E:31:20:09:B6"
#define OBD_BLE_PIN      1234   // BLE passkey / pairing PIN
#define OBD_FORCE_PAIR   0      // 1 = always pair with PIN; 0 = pair only if needed

// Common Chinese BLE ELM327 clones use FFF0 / FFF1 / FFF2.
// Some (OBDLink / iOS-friendly) use 18F0 / 2AF0 / 2AF1.
// Nordic UART is another option. The sketch tries all presets.
enum BleUuidPreset {
  UUID_FFF0 = 0,
  UUID_18F0,
  UUID_NORDIC_UART,
  UUID_PRESET_COUNT
};

#define BLE_SCAN_SECONDS   8
#define ELM_TIMEOUT_MS     800
#define PID_INTERVAL_MS    0     // 0 = as fast as ELM answers
// BLE TX power dBm (ESP steps ~3 dBm). Was +9 (max); +3 is cooler, still OK in-car.
#define BLE_TX_POWER_DBM   3
// CPU MHz while running gauge (160 default). 80 is enough for BLE+LCD UI.
#define CPU_FREQ_MHZ       80

// Harrier 1999 / Lexus RX 1st gen: ISO 9141-2 (K-Line pin 7).
// ELM: 3 = ISO 9141-2. Do NOT use ATSP0 (auto) — wakes other ECUs harder.
#define OBD_ELM_PROTOCOL   "3"

// ========================= DISPLAY / BUTTON =========================
// Waveshare BOOT button = GPIO9 (to GND, already on board).
// Or wire any button: GPIO <-> GND, leave INPUT_PULLUP.
#define BTN_PIN            9
#define BTN_ACTIVE_LOW     1
#define BTN_DEBOUNCE_MS    40
#define BTN_LONG_MS        1500
#define PAGE_COUNT         8
#define PAGE_OVERVIEW      0
#define PAGE_BAT           1
#define PAGE_RPM           2
#define PAGE_COOLANT       3
#define PAGE_AMBIENT       4
#define PAGE_TANK          5
#define PAGE_TRIP          6
#define PAGE_SCREEN_OFF    7
#define UI_REFRESH_MS      250  // redraw live page at most this often
// RPM display smoothing (visual only; OBD poll rate unchanged).
#define RPM_SMOOTH_GAIN    12.0f   // lerp toward new reading (~250 ms settle)

// ========================= TRIP / FUEL =========================
#define TRIP_MIN_KM        0.2f    // min distance before avg L/100 is shown
#define TRIP_SAVE_MS       15000   // autosave interval to NVS
// Don't integrate fuel unless engine is actually spinning (key-ON / MAF noise).
#define TRIP_MIN_RPM       400.0f
// Tank capacity for liters estimate from PID 012F (%).
// Toyota Harrier XU10 / RX ~65 L — поправь при необходимости.
#define TANK_CAPACITY_L    65.0f

// ========================= MOCK UI =========================
// Tap BOOT 5 times quickly to enter/exit mock telemetry (no OBD needed).
#define MOCK_TAP_COUNT     5
#define MOCK_TAP_WINDOW_MS 2500
#define MOCK_BOOT_WAIT_MS  5000  // at boot: wait for mock taps before BLE scan

// ========================= WIFI OTA (GitHub pull) =========================
// Long-press Overview → connect to phone hotspot → check GitHub for newer bin.
#define FIRMWARE_VERSION   "1.2.13"
#define OTA_WIFI_SSID      "13T"
#define OTA_WIFI_PASS      "12121212"
#define OTA_WIFI_TIMEOUT_MS 45000
// Prefer raw GitHub + ?t= cache-bust (jsDelivr @main can lag for hours).
#define OTA_GITHUB_BASE \
  "https://raw.githubusercontent.com/Vannyhas/ESP32-To-OBD-II/refs/heads/main/firmware/"

// Legacy AP upload (unused by UI; kept for emergency tools)
#define OTA_AP_SSID        "ESP32-OBD-OTA"
#define OTA_AP_PASS        "12345678"   // WPA: min 8 chars
