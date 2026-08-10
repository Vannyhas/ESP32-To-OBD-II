#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

#include "config.h"
#include "BleElmClient.h"
#include "Elm327.h"
#include "TripComputer.h"
#include "OtaPortal.h"
#include "OtaPull.h"

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO);
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, LCD_RST, LCD_ROTATION, true /* IPS */,
    172 /* width */, 320 /* height */,
    34 /* col offset 1 */, 0 /* row offset 1 */,
    34 /* col offset 2 */, 0 /* row offset 2 */);

Elm327 elm;
ObdData telemetry;

enum UiState { UI_BOOT, UI_SCAN, UI_CONNECT, UI_INIT, UI_LIVE, UI_ERROR, UI_MOCK, UI_OTA };
UiState uiState = UI_BOOT;
String statusMsg = "Boot";
uint8_t page = PAGE_BAT;
bool layoutDirty = true;
bool valuesDirty = true;
bool displayOff = false;
int8_t drawnPage = -1;
unsigned long lastPidMs = 0;
unsigned long lastReconnectMs = 0;
unsigned long lastMockMs = 0;
unsigned long lastTripMs = 0;

bool btnStable = false;
bool btnLastRaw = false;
unsigned long btnChangeMs = 0;
unsigned long btnPressStartMs = 0;
bool btnLongHandled = false;

uint8_t rapidTaps = 0;
unsigned long rapidTapStartMs = 0;

char lastOverview[4][24];
char lastBigValue[24];
char lastFuelLines[4][28];
char lastTankLines[3][28];

void showStatus(const char* title, const char* line2 = nullptr);
void updateMockTelemetry();
void enterMockMode();
void exitMockMode();
void invalidateUi(bool fullLayout);
void resetTripWithFeedback();
void enterOtaMode();
void exitOtaMode();
void drawOtaScreen();
void setDisplayPower(bool on);
void goToBatScreen();

bool readButtonRaw() {
  const int v = digitalRead(BTN_PIN);
#if BTN_ACTIVE_LOW
  return v == LOW;
#else
  return v == HIGH;
#endif
}

// Debounced level tracking; sets edges via out params.
void pollButton(bool& pressedEdge, bool& releasedEdge, bool& longPress) {
  pressedEdge = false;
  releasedEdge = false;
  longPress = false;

  const bool raw = readButtonRaw();
  const unsigned long now = millis();

  if (raw != btnLastRaw) {
    btnLastRaw = raw;
    btnChangeMs = now;
  }
  if ((now - btnChangeMs) < BTN_DEBOUNCE_MS) {
    return;
  }

  if (raw != btnStable) {
    btnStable = raw;
    if (btnStable) {
      pressedEdge = true;
      btnPressStartMs = now;
      btnLongHandled = false;
    } else {
      releasedEdge = true;
    }
  }

  if (btnStable && !btnLongHandled &&
      (now - btnPressStartMs) >= BTN_LONG_MS) {
    btnLongHandled = true;
    longPress = true;
  }
}

void invalidateUi(bool fullLayout) {
  if (fullLayout) {
    layoutDirty = true;
    drawnPage = -1;
    memset(lastOverview, 0, sizeof(lastOverview));
    memset(lastFuelLines, 0, sizeof(lastFuelLines));
    memset(lastTankLines, 0, sizeof(lastTankLines));
    lastBigValue[0] = 0;
  }
  valuesDirty = true;
}

void nextPage() {
  if (displayOff) return;
  page = (page + 1) % PAGE_COUNT;
  invalidateUi(true);
  Serial.printf("[UI] page=%u\n", page);
}

void goToBatScreen() {
  page = PAGE_BAT;
  invalidateUi(true);
}

void setDisplayPower(bool on) {
  displayOff = !on;
  if (on) {
    digitalWrite(LCD_BL, HIGH);
    invalidateUi(true);
    Serial.println("[UI] display ON");
  } else {
    gfx->fillScreen(RGB565_BLACK);
    digitalWrite(LCD_BL, LOW);
    drawnPage = -1;
    Serial.println("[UI] display OFF");
  }
}

void resetTripWithFeedback() {
  trip.reset();
  invalidateUi(true);
  showStatus("Trip RESET", "dist & fuel cleared");
  delay(700);
  invalidateUi(true);
}

void drawOtaStatus(const char* title, const char* line2) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillRect(0, 0, 5, SCREEN_HEIGHT, RGB565_ORANGE);
  gfx->setTextColor(RGB565_ORANGE);
  gfx->setTextSize(2);
  gfx->setCursor(14, 6);
  gfx->println(F("OTA Update"));

  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_LIGHTGREY);
  gfx->setCursor(14, 36);
  gfx->printf("FW  %s", OtaPull::firmwareVersion());

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(14, 64);
  gfx->println(title ? title : "");
  if (line2 && line2[0]) {
    gfx->setTextSize(1);
    gfx->setTextColor(RGB565_YELLOW);
    gfx->setCursor(14, 100);
    gfx->println(line2);
  }
  drawnPage = -1;
}

void onOtaPullStatus(OtaPull::Status st, const char* detail, int progressPct) {
  switch (st) {
    case OtaPull::Status::ConnectingWifi:
      drawOtaStatus("Wi-Fi...", detail);
      break;
    case OtaPull::Status::WifiFailed:
      drawOtaStatus("No Wi-Fi", detail);
      break;
    case OtaPull::Status::FetchingManifest:
      drawOtaStatus("Checking...", detail);
      break;
    case OtaPull::Status::ManifestFailed:
      drawOtaStatus("Manifest fail", detail);
      break;
    case OtaPull::Status::UpToDate:
      drawOtaStatus("Up to date", detail);
      break;
    case OtaPull::Status::Downloading: {
      char title[24];
      if (progressPct >= 0) {
        snprintf(title, sizeof(title), "DL %d%%", progressPct);
      } else {
        snprintf(title, sizeof(title), "Download");
      }
      drawOtaStatus(title, detail);
      break;
    }
    case OtaPull::Status::FlashFailed:
      drawOtaStatus("Flash fail", detail);
      break;
    case OtaPull::Status::Rebooting:
      drawOtaStatus("Rebooting...", detail);
      break;
    default:
      break;
  }
}

void enterOtaMode() {
  trip.save();
  bleObd.disconnect();
  NimBLEDevice::deinit(true);
  delay(500);  // let BT stack release radio before Wi‑Fi

  uiState = UI_OTA;
  drawOtaStatus("Starting...", OTA_WIFI_SSID);
  Serial.println("[UI] OTA pull start");

  // Blocks; reboots on success. On fail / up-to-date — restore BLE.
  const bool updating = OtaPull::checkAndUpdate(onOtaPullStatus);
  if (updating) return;  // reboot pending

  delay(1500);
  bleObd.begin("ESP32-OBD-LCD");
  uiState = UI_ERROR;
  statusMsg = "OTA done";
  showStatus("OTA finished", "Reconnecting OBD...");
  lastReconnectMs = 0;
  Serial.println("[UI] OTA pull end (no reboot)");
}

void exitOtaMode() {
  // Pull OTA is blocking; this is only a safety path.
  OtaPortal::stop();
  delay(50);
  bleObd.begin("ESP32-OBD-LCD");
  uiState = UI_ERROR;
  statusMsg = "Left OTA";
  showStatus("OTA closed", "Reconnecting OBD...");
  lastReconnectMs = 0;
  Serial.println("[UI] OTA mode OFF");
}

void updateMockTelemetry() {
  const float t = millis() / 1000.0f;
  telemetry.voltage = 12.2f + 0.6f * sinf(t * 0.7f);
  telemetry.rpm = 850.0f + 2200.0f * (0.5f + 0.5f * sinf(t * 1.3f));
  telemetry.coolantC = 78.0f + 12.0f * sinf(t * 0.25f);
  telemetry.speedKmh = 35.0f + 45.0f * (0.5f + 0.5f * sinf(t * 0.55f));
  telemetry.throttlePct = 10.0f + 55.0f * (0.5f + 0.5f * sinf(t * 1.1f));
  telemetry.engineLoad = 15.0f + 50.0f * (0.5f + 0.5f * sinf(t * 0.8f));
  // ~2.5-6 g/s MAF → plausible fuel flow for mock
  telemetry.mafGps = 2.5f + 3.5f * (0.5f + 0.5f * sinf(t * 0.9f));
  telemetry.fuelRateLph = NAN;
  telemetry.fuelLevelPct = 55.0f + 20.0f * sinf(t * 0.15f);
  telemetry.ambientC = 24.0f + 8.0f * sinf(t * 0.2f);
  telemetry.valid = true;
}

void enterMockMode() {
  bleObd.disconnect();
  uiState = UI_MOCK;
  goToBatScreen();
  updateMockTelemetry();
  setDisplayPower(true);
  showStatus("MOCK MODE", "hold RPM = exit");
  delay(400);
  invalidateUi(true);
  Serial.println("[UI] Mock mode ON");
}

void exitMockMode() {
  uiState = UI_ERROR;
  statusMsg = "Left mock";
  showStatus("Mock OFF", "Reconnecting OBD...");
  Serial.println("[UI] Mock mode OFF");
  lastReconnectMs = 0;
  trip.save();
}

void showStatus(const char* title, const char* line2) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_CYAN);
  gfx->setTextSize(2);
  gfx->setCursor(8, 8);
  gfx->println(F("ESP32 OBD-II"));
  gfx->drawFastHLine(0, 30, SCREEN_WIDTH, RGB565_DARKGREY);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(8, 50);
  gfx->println(title);
  if (line2) {
    gfx->setTextColor(RGB565_YELLOW);
    gfx->setTextSize(1);
    gfx->setCursor(8, 90);
    gfx->println(line2);
  }
  drawnPage = -1;
}

void drawPageDots(uint16_t accent) {
  for (uint8_t i = 0; i < PAGE_COUNT; i++) {
    int x = SCREEN_WIDTH - 86 + i * 12;
    int y = SCREEN_HEIGHT - 12;
    if (i == page) {
      gfx->fillCircle(x, y, 3, accent);
    } else {
      gfx->drawCircle(x, y, 3, RGB565_DARKGREY);
    }
  }

  if (uiState == UI_MOCK) {
    gfx->setTextSize(1);
    gfx->setTextColor(RGB565_MAGENTA);
    gfx->setCursor(8, SCREEN_HEIGHT - 14);
    gfx->print(F("MOCK"));
  }
}

static void fmtOrDash(char* out, size_t n, const char* prefix, float v, int decimals,
                      const char* suffix) {
  if (isnan(v)) {
    snprintf(out, n, "%s--%s", prefix, suffix);
  } else if (decimals <= 0) {
    snprintf(out, n, "%s%.0f%s", prefix, v, suffix);
  } else {
    snprintf(out, n, "%s%.*f%s", prefix, decimals, v, suffix);
  }
}

static void paintTextInRect(int x, int y, int w, int h, const char* text,
                            uint8_t textSize, uint16_t color) {
  gfx->fillRect(x, y, w, h, RGB565_BLACK);
  gfx->setTextSize(textSize);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(text);
}

static char valueUnit[16];
static uint16_t valueAccent = RGB565_WHITE;

static int textWidthPx(const char* text, uint8_t size) {
  return (int)strlen(text) * 6 * size;
}

static uint8_t fitTextSize(const char* text, int maxWidth, uint8_t maxSize,
                           uint8_t minSize) {
  uint8_t size = maxSize;
  while (size > minSize && textWidthPx(text, size) > maxWidth) {
    size--;
  }
  return size;
}

void drawOverviewChrome() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillRect(0, 0, 5, SCREEN_HEIGHT, RGB565_CYAN);
  gfx->setTextColor(RGB565_CYAN);
  gfx->setTextSize(2);
  gfx->setCursor(14, 6);
  gfx->println(F("Overview"));
  drawPageDots(RGB565_CYAN);
  memset(lastOverview, 0, sizeof(lastOverview));
}

void drawOverviewValues() {
  char lines[4][24];
  fmtOrDash(lines[0], sizeof(lines[0]), "", telemetry.voltage, 1, " V");
  fmtOrDash(lines[1], sizeof(lines[1]), "", telemetry.coolantC, 0, " C");
  fmtOrDash(lines[2], sizeof(lines[2]), "", telemetry.engineLoad, 0, " %");

  const float avg = trip.avgLPer100();
  if (isnan(avg)) {
    snprintf(lines[3], sizeof(lines[3]), "--.-");
  } else {
    snprintf(lines[3], sizeof(lines[3]), "%.1f", avg);
  }

  const char* labels[4] = {"BAT", "TEMP", "LOAD", "AVG"};
  const char* suffixes[4] = {"", "", "", " L/100"};
  const int ys[4] = {34, 66, 98, 130};

  for (int i = 0; i < 4; i++) {
    char row[28];
    snprintf(row, sizeof(row), "%s  %s%s", labels[i], lines[i], suffixes[i]);
    if (strcmp(row, lastOverview[i]) == 0) continue;
    strncpy(lastOverview[i], row, sizeof(lastOverview[i]) - 1);
    lastOverview[i][sizeof(lastOverview[i]) - 1] = 0;
    paintTextInRect(14, ys[i], SCREEN_WIDTH - 22, 28, row, 2, RGB565_WHITE);
  }
}

void drawValueChrome(const char* label, const char* unit, uint16_t accent) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillRect(0, 0, 5, SCREEN_HEIGHT, accent);

  gfx->setTextSize(2);
  gfx->setTextColor(accent);
  gfx->setCursor(14, 8);
  gfx->print(label);

  strncpy(valueUnit, unit, sizeof(valueUnit) - 1);
  valueUnit[sizeof(valueUnit) - 1] = 0;
  valueAccent = accent;

  drawPageDots(accent);
  lastBigValue[0] = 0;
}

void drawBigValue(float value, int decimals) {
  char buf[24];
  if (isnan(value)) {
    snprintf(buf, sizeof(buf), "--");
  } else {
    snprintf(buf, sizeof(buf), "%.*f", decimals, value);
  }
  if (strcmp(buf, lastBigValue) == 0) return;
  strncpy(lastBigValue, buf, sizeof(lastBigValue) - 1);
  lastBigValue[sizeof(lastBigValue) - 1] = 0;

  // Clear main stage (leave top label + bottom dots)
  gfx->fillRect(10, 32, SCREEN_WIDTH - 14, SCREEN_HEIGHT - 48, RGB565_BLACK);

  const uint8_t size = fitTextSize(buf, SCREEN_WIDTH - 70, 9, 5);
  const int tw = textWidthPx(buf, size);
  const int th = 8 * size;
  const int x = (SCREEN_WIDTH - tw) / 2;
  const int y = 32 + (SCREEN_HEIGHT - 48 - th) / 2;

  gfx->setTextSize(size);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(x, y);
  gfx->print(buf);

  // Unit to the right of the number
  gfx->setTextSize(2);
  gfx->setTextColor(valueAccent);
  int ux = x + tw + 8;
  int uy = y + th - 18;
  if (ux + textWidthPx(valueUnit, 2) > SCREEN_WIDTH - 4) {
    ux = x;
    uy = y + th + 4;
  }
  gfx->setCursor(ux, uy);
  gfx->print(valueUnit);
}

void drawFuelChrome() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillRect(0, 0, 5, SCREEN_HEIGHT, RGB565_YELLOW);
  gfx->setTextColor(RGB565_YELLOW);
  gfx->setTextSize(2);
  gfx->setCursor(14, 6);
  gfx->println(F("Trip"));
  drawPageDots(RGB565_YELLOW);
  memset(lastFuelLines, 0, sizeof(lastFuelLines));
}

void drawFuelValues() {
  char avgBuf[16];
  const float avg = trip.avgLPer100();
  if (isnan(avg)) {
    snprintf(avgBuf, sizeof(avgBuf), "--.-");
  } else {
    snprintf(avgBuf, sizeof(avgBuf), "%.1f", avg);
  }

  char lines[3][28];
  snprintf(lines[0], sizeof(lines[0]), "%s", avgBuf);
  snprintf(lines[1], sizeof(lines[1]), "%.2f km", trip.distanceKm());
  snprintf(lines[2], sizeof(lines[2]), "%.3f L", trip.fuelLiters());

  // Hero AVG
  if (strcmp(lines[0], lastFuelLines[0]) != 0) {
    strncpy(lastFuelLines[0], lines[0], sizeof(lastFuelLines[0]) - 1);
    lastFuelLines[0][sizeof(lastFuelLines[0]) - 1] = 0;
    gfx->fillRect(10, 30, SCREEN_WIDTH - 14, 78, RGB565_BLACK);

    gfx->setTextSize(1);
    gfx->setTextColor(RGB565_YELLOW);
    gfx->setCursor(14, 32);
    gfx->print(F("AVG"));

    const uint8_t size = fitTextSize(lines[0], SCREEN_WIDTH - 110, 7, 4);
    const int tw = textWidthPx(lines[0], size);
    const int th = 8 * size;
    const int x = 20;
    const int y = 48;
    gfx->setTextSize(size);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setCursor(x, y);
    gfx->print(lines[0]);

    gfx->setTextSize(2);
    gfx->setTextColor(RGB565_YELLOW);
    gfx->setCursor(x + tw + 8, y + th - 18);
    gfx->print(F("L/100km"));
  }

  for (int i = 1; i < 3; i++) {
    if (strcmp(lines[i], lastFuelLines[i]) == 0) continue;
    strncpy(lastFuelLines[i], lines[i], sizeof(lastFuelLines[i]) - 1);
    lastFuelLines[i][sizeof(lastFuelLines[i]) - 1] = 0;
    const int y = (i == 1) ? 118 : 142;
    paintTextInRect(14, y, SCREEN_WIDTH - 22, 22, lines[i], 2, RGB565_LIGHTGREY);
  }
}

void drawTankChrome() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillRect(0, 0, 5, SCREEN_HEIGHT, RGB565_GREENYELLOW);
  gfx->setTextColor(RGB565_GREENYELLOW);
  gfx->setTextSize(2);
  gfx->setCursor(14, 6);
  gfx->println(F("Tank"));
  drawPageDots(RGB565_GREENYELLOW);
  memset(lastTankLines, 0, sizeof(lastTankLines));
}

void drawTankValues() {
  char pctBuf[16];
  char litBuf[28];
  const float pct = telemetry.fuelLevelPct;

  if (isnan(pct)) {
    snprintf(pctBuf, sizeof(pctBuf), "--");
    snprintf(litBuf, sizeof(litBuf), "--.- L / %.0f L", TANK_CAPACITY_L);
  } else {
    snprintf(pctBuf, sizeof(pctBuf), "%.0f", pct);
    const float liters = TANK_CAPACITY_L * (pct / 100.0f);
    snprintf(litBuf, sizeof(litBuf), "%.1f L / %.0f L", liters, TANK_CAPACITY_L);
  }

  if (strcmp(pctBuf, lastTankLines[0]) != 0) {
    strncpy(lastTankLines[0], pctBuf, sizeof(lastTankLines[0]) - 1);
    lastTankLines[0][sizeof(lastTankLines[0]) - 1] = 0;
    gfx->fillRect(10, 30, SCREEN_WIDTH - 14, 90, RGB565_BLACK);

    const uint8_t size = fitTextSize(pctBuf, SCREEN_WIDTH - 80, 9, 5);
    const int tw = textWidthPx(pctBuf, size);
    const int th = 8 * size;
    const int x = (SCREEN_WIDTH - tw) / 2 - 10;
    const int y = 36 + (90 - th) / 2;
    gfx->setTextSize(size);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setCursor(x, y);
    gfx->print(pctBuf);

    gfx->setTextSize(3);
    gfx->setTextColor(RGB565_GREENYELLOW);
    gfx->setCursor(x + tw + 6, y + th - 28);
    gfx->print('%');
  }

  if (strcmp(litBuf, lastTankLines[1]) != 0) {
    strncpy(lastTankLines[1], litBuf, sizeof(lastTankLines[1]) - 1);
    lastTankLines[1][sizeof(lastTankLines[1]) - 1] = 0;
    paintTextInRect(14, 130, SCREEN_WIDTH - 22, 24, litBuf, 2, RGB565_LIGHTGREY);
  }
}

void renderLive() {
  const bool needChrome = layoutDirty || drawnPage != (int8_t)page;

  if (needChrome) {
    switch (page) {
      case PAGE_OVERVIEW: drawOverviewChrome(); break;
      case PAGE_BAT: drawValueChrome("BAT LVL", "V", RGB565_GREEN); break;
      case PAGE_RPM: drawValueChrome("RPM", "rpm", RGB565_ORANGE); break;
      case PAGE_COOLANT: drawValueChrome("Coolant", "C", RGB565_RED); break;
      case PAGE_AMBIENT: drawValueChrome("Outside", "C", RGB565_CYAN); break;
      case PAGE_TANK: drawTankChrome(); break;
      case PAGE_TRIP: drawFuelChrome(); break;
      default:
        page = PAGE_OVERVIEW;
        drawOverviewChrome();
        break;
    }
    drawnPage = (int8_t)page;
    layoutDirty = false;
    valuesDirty = true;
  }

  if (!valuesDirty) return;
  valuesDirty = false;

  switch (page) {
    case PAGE_OVERVIEW: drawOverviewValues(); break;
    case PAGE_BAT: drawBigValue(telemetry.voltage, 1); break;
    case PAGE_RPM: drawBigValue(telemetry.rpm, 0); break;
    case PAGE_COOLANT: drawBigValue(telemetry.coolantC, 0); break;
    case PAGE_AMBIENT: drawBigValue(telemetry.ambientC, 0); break;
    case PAGE_TANK: drawTankValues(); break;
    case PAGE_TRIP: drawFuelValues(); break;
    default: break;
  }
}

bool connectAndInit() {
  if (uiState == UI_MOCK) return true;

  uiState = UI_SCAN;
  showStatus("Scanning BLE...", "BOOT x5 = MOCK");

  if (!bleObd.connectToObd(OBD_TARGET_NAME)) {
    uiState = UI_ERROR;
    statusMsg = "OBD not found";
    showStatus("OBD not found", "BOOT x5 = MOCK");
    return false;
  }

  uiState = UI_INIT;
  showStatus("ELM327 init...", bleObd.isConnected() ? "Linked" : "");

  if (!elm.begin(bleObd)) {
    uiState = UI_ERROR;
    statusMsg = "ELM init fail";
    showStatus("ELM init failed", "BOOT x5 = MOCK");
    bleObd.disconnect();
    return false;
  }

  uiState = UI_LIVE;
  lastPidMs = 0;
  setDisplayPower(true);
  goToBatScreen();
  showStatus("Connected!", "hold BAT=sleep");
  delay(400);
  invalidateUi(true);
  return true;
}

bool handleRapidTapForMock() {
  // While MOCK is on, 5x taps do nothing — exit only via long-press on RPM.
  if (uiState == UI_MOCK) {
    return false;
  }

  const unsigned long now = millis();
  if (rapidTaps == 0 || (now - rapidTapStartMs) > MOCK_TAP_WINDOW_MS) {
    rapidTaps = 1;
    rapidTapStartMs = now;
  } else {
    rapidTaps++;
  }

  Serial.printf("[UI] rapid taps=%u\n", rapidTaps);

  if (rapidTaps >= MOCK_TAP_COUNT) {
    rapidTaps = 0;
    enterMockMode();
    return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32-C6 BLE OBD LCD 1.47 ===");

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  pinMode(BTN_PIN, INPUT_PULLUP);
  btnLastRaw = readButtonRaw();
  btnStable = btnLastRaw;
  btnChangeMs = millis();
  memset(lastOverview, 0, sizeof(lastOverview));
  memset(lastFuelLines, 0, sizeof(lastFuelLines));
  memset(lastTankLines, 0, sizeof(lastTankLines));
  lastBigValue[0] = 0;

  trip.begin();

  if (!gfx->begin()) {
    Serial.println("LCD init failed");
  }
  gfx->fillScreen(RGB565_BLACK);

  bleObd.begin("ESP32-OBD-LCD");

  showStatus("BOOT x5 = MOCK", "hold Overview=OTA");
  const unsigned long waitStart = millis();
  while (millis() - waitStart < MOCK_BOOT_WAIT_MS) {
    bool pe = false, re = false, lp = false;
    pollButton(pe, re, lp);
    if (pe && handleRapidTapForMock()) break;
    delay(10);
  }

  if (uiState != UI_MOCK) {
    connectAndInit();
  }
}

void loop() {
  bool pressedEdge = false, releasedEdge = false, longPress = false;
  pollButton(pressedEdge, releasedEdge, longPress);

  if (uiState == UI_OTA) {
    OtaPortal::loop();
    if (longPress || (releasedEdge && !btnLongHandled &&
                      (millis() - btnPressStartMs) < BTN_LONG_MS)) {
      exitOtaMode();
      goToBatScreen();
    }
    delay(2);
    return;
  }

  // Display sleep: long-press wakes; ignore other UI
  if (displayOff) {
    if (longPress) {
      rapidTaps = 0;
      setDisplayPower(true);
    }
    // Keep trip/OBD alive quietly while screen is off
    if (uiState == UI_MOCK) {
      if (millis() - lastMockMs >= 500) {
        lastMockMs = millis();
        updateMockTelemetry();
      }
    } else if (uiState == UI_LIVE && bleObd.isConnected()) {
      if (millis() - lastPidMs >= PID_INTERVAL_MS) {
        lastPidMs = millis();
        elm.pollForPage(telemetry, page);
      }
    } else if (uiState == UI_ERROR || (uiState == UI_LIVE && !bleObd.isConnected())) {
      if (millis() - lastReconnectMs > 5000) {
        lastReconnectMs = millis();
        bleObd.disconnect();
        connectAndInit();
      }
    }
    if (uiState == UI_LIVE || uiState == UI_MOCK) {
      if (millis() - lastTripMs >= 500) {
        lastTripMs = millis();
        trip.update(telemetry, uiState == UI_MOCK);
      }
    }
    delay(20);
    return;
  }

  if (longPress) {
    rapidTaps = 0;
    if (uiState == UI_MOCK && page == PAGE_RPM) {
      exitMockMode();
      return;
    }
    if (uiState == UI_LIVE || uiState == UI_MOCK) {
      if (page == PAGE_TRIP) {
        resetTripWithFeedback();
      } else if (page == PAGE_OVERVIEW) {
        enterOtaMode();
        return;  // don't fall through into BLE reconnect
      } else if (page == PAGE_BAT) {
        setDisplayPower(false);
        return;
      }
    }
  }

  if (pressedEdge) {
    handleRapidTapForMock();
  }

  if (releasedEdge && !btnLongHandled &&
      (uiState == UI_LIVE || uiState == UI_MOCK)) {
    if ((millis() - btnPressStartMs) < BTN_LONG_MS) {
      nextPage();
    }
  }

  // OTA / sleep must never be treated as "BLE lost"
  if (uiState == UI_OTA || displayOff) {
    delay(2);
    return;
  }

  const bool liveLike = (uiState == UI_LIVE || uiState == UI_MOCK);

  if (uiState == UI_MOCK) {
    if (millis() - lastMockMs >= 150) {
      lastMockMs = millis();
      updateMockTelemetry();
      valuesDirty = true;
    }
  } else if (uiState == UI_LIVE && !bleObd.isConnected()) {
    uiState = UI_ERROR;
    statusMsg = "OBD lost";
  } else if (uiState == UI_ERROR) {
    if (millis() - lastReconnectMs > 5000) {
      lastReconnectMs = millis();
      showStatus("Reconnecting...", "BOOT x5 = MOCK");
      bleObd.disconnect();
      connectAndInit();
    }
    delay(20);
    return;
  } else if (millis() - lastPidMs >= PID_INTERVAL_MS) {
    lastPidMs = millis();
    if (elm.pollForPage(telemetry, page)) {
      valuesDirty = true;
    }
  }

  if (liveLike && (millis() - lastTripMs >= 200)) {
    lastTripMs = millis();
    trip.update(telemetry, uiState == UI_MOCK);
    if (page == PAGE_OVERVIEW || page == PAGE_TANK || page == PAGE_TRIP) {
      valuesDirty = true;
    }
  }

  if (liveLike && (layoutDirty || valuesDirty)) {
    renderLive();
  }

  delay(10);
}
