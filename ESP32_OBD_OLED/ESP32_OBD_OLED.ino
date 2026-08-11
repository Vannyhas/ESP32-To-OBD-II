#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <canvas/Arduino_Canvas.h>
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
constexpr int VALUE_STAGE_X = 10;
constexpr int VALUE_STAGE_Y = 32;
constexpr int VALUE_STAGE_W = SCREEN_WIDTH - 14;
constexpr int VALUE_STAGE_H = SCREEN_HEIGHT - 48;
Arduino_Canvas *stageCanvas = new Arduino_Canvas(
    VALUE_STAGE_W, VALUE_STAGE_H, gfx, VALUE_STAGE_X, VALUE_STAGE_Y);

Elm327 elm;
ObdData telemetry;

enum UiState { UI_BOOT, UI_SCAN, UI_CONNECT, UI_INIT, UI_LIVE, UI_ERROR, UI_MOCK, UI_OTA };
UiState uiState = UI_BOOT;
String statusMsg = "Boot";
uint8_t page = PAGE_OVERVIEW;
bool layoutDirty = true;
bool valuesDirty = true;
bool displayOff = false;
int8_t drawnPage = -1;
unsigned long lastPidMs = 0;
unsigned long lastReconnectMs = 0;
unsigned long lastMockMs = 0;
unsigned long lastTripMs = 0;
Preferences uiPrefs;

bool btnStable = false;
bool btnLastRaw = false;
unsigned long btnChangeMs = 0;
unsigned long btnPressStartMs = 0;
bool btnLongHandled = false;

// Queued while blocked in slow ELM BLE waits (ISO 9141).
bool pendingNextPage = false;
bool pendingLongPress = false;

uint8_t rapidTaps = 0;
unsigned long rapidTapStartMs = 0;

char lastOverview[4][24];
char lastBigValue[24];
char lastFuelLines[4][28];
char lastTankLines[3][28];

float rpmDisplay = NAN;
float rpmTarget = NAN;
unsigned long rpmSmoothLastMs = 0;
int lastRpmShown = -1;

void resetRpmDisplay() {
  rpmDisplay = telemetry.rpm;
  rpmTarget = telemetry.rpm;
  rpmSmoothLastMs = 0;
  lastRpmShown = -1;
}

// Visual interpolation between OBD RPM samples.
void updateRpmDisplay() {
  if (page != PAGE_RPM) {
    rpmDisplay = telemetry.rpm;
    rpmTarget = telemetry.rpm;
    rpmSmoothLastMs = 0;
    return;
  }

  const unsigned long now = millis();
  if (isnan(telemetry.rpm)) {
    rpmDisplay = NAN;
    rpmTarget = NAN;
    rpmSmoothLastMs = now;
    return;
  }

  rpmTarget = telemetry.rpm;

  if (rpmSmoothLastMs == 0 || isnan(rpmDisplay)) {
    rpmDisplay = rpmTarget;
    rpmSmoothLastMs = now;
    return;
  }

  float dt = (now - rpmSmoothLastMs) / 1000.0f;
  if (dt <= 0.0f || dt > 0.5f) {
    rpmSmoothLastMs = now;
    return;
  }
  rpmSmoothLastMs = now;

  const float delta = rpmTarget - rpmDisplay;
  const float alpha = RPM_SMOOTH_GAIN * dt;
  if (alpha >= 1.0f || fabsf(delta) < 0.5f) {
    rpmDisplay = rpmTarget;
  } else {
    rpmDisplay += delta * alpha;
  }
}

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
void saveUiState();
void restoreUiState();
void applyUiState(uint8_t newPage, bool screenOff);
void drawScreenOffChrome();

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
  if (fullLayout) {
    resetRpmDisplay();
  }
  valuesDirty = true;
}

void nextPage() {
  if (displayOff) return;
  const uint8_t next = (page + 1) % PAGE_COUNT;
  applyUiState(next, next == PAGE_SCREEN_OFF);
  Serial.printf("[UI] page=%u\n", page);
}

void goToBatScreen() {
  applyUiState(PAGE_OVERVIEW, false);
}

void setDisplayPower(bool on) {
  displayOff = !on;
  if (on) {
    digitalWrite(LCD_BL, HIGH);
    invalidateUi(true);
    saveUiState();
    Serial.println("[UI] display ON");
  } else {
    gfx->fillScreen(RGB565_BLACK);
    digitalWrite(LCD_BL, LOW);
    drawnPage = -1;
    saveUiState();
    Serial.println("[UI] display OFF");
  }
}

void saveUiState() {
  uiPrefs.putUChar("page", page);
  uiPrefs.putBool("off", displayOff);
}

void restoreUiState() {
  uint8_t savedPage = uiPrefs.getUChar("page", PAGE_OVERVIEW);
  if (savedPage >= PAGE_COUNT) savedPage = PAGE_OVERVIEW;
  const bool savedOff = uiPrefs.getBool("off", false);
  page = savedPage;
  displayOff = false;
  invalidateUi(true);
  setDisplayPower(!savedOff);
}

void applyUiState(uint8_t newPage, bool screenOff) {
  if (newPage >= PAGE_COUNT) newPage = PAGE_OVERVIEW;
  page = newPage;
  invalidateUi(true);
  if (screenOff) {
    setDisplayPower(false);
  } else if (displayOff) {
    setDisplayPower(true);
  } else {
    saveUiState();
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
  bleObd.end();
  telemetry = ObdData{};  // don't carry mock sensors across Wi‑Fi OTA
  delay(500);  // let BT stack release radio before Wi‑Fi

  uiState = UI_OTA;
  drawOtaStatus("Starting...", OTA_WIFI_SSID);
  Serial.println("[UI] OTA pull start");

  // Blocks; reboots on success. On fail / up-to-date — restore BLE.
  const bool updating = OtaPull::checkAndUpdate(onOtaPullStatus);
  if (updating) return;  // reboot pending

  delay(1500);
  telemetry = ObdData{};
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
  telemetry.intakeC = 28.0f + 10.0f * sinf(t * 0.22f);
  telemetry.ambientC = NAN;
  telemetry.valid = true;
}

void handleLongPressActions() {
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
    } else if (page == PAGE_SCREEN_OFF) {
      setDisplayPower(true);
    }
  }
}

// Called from BleElmClient while waiting for ELM response — keeps BOOT snappy.
void elmUiYield() {
  bool pe = false, re = false, lp = false;
  pollButton(pe, re, lp);
  if (pe) {
    handleRapidTapForMock();
  }
  if (lp) {
    pendingLongPress = true;
  }
  if (re && !btnLongHandled && (millis() - btnPressStartMs) < BTN_LONG_MS) {
    if (uiState == UI_LIVE || uiState == UI_MOCK) {
      pendingNextPage = true;
    }
  }
}

void enterMockMode() {
  bleObd.end();  // full radio off — no heat in mock
  uiState = UI_MOCK;
  restoreUiState();
  updateMockTelemetry();
  resetRpmDisplay();
  showStatus("MOCK MODE", "hold RPM = exit");
  delay(400);
  invalidateUi(true);
  Serial.println("[UI] Mock mode ON (BLE off)");
}

void exitMockMode() {
  uiState = UI_ERROR;
  statusMsg = "Left mock";
  showStatus("Mock OFF", "Starting BLE...");
  Serial.println("[UI] Mock mode OFF");
  lastReconnectMs = 0;
  trip.save();
  // Drop fake sensors so live OBD doesn't briefly inherit mock MAF/RPM.
  telemetry = ObdData{};
  resetRpmDisplay();
  bleObd.begin("ESP32-OBD-LCD");
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

void drawScreenOffChrome() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillRect(0, 0, 5, SCREEN_HEIGHT, RGB565_DARKGREY);
  gfx->setTextColor(RGB565_DARKGREY);
  gfx->setTextSize(2);
  gfx->setCursor(14, 8);
  gfx->print(F("Screen Off"));

  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_LIGHTGREY);
  gfx->setCursor(14, 44);
  gfx->print(F("Trip and BLE keep running"));
  gfx->setCursor(14, 60);
  gfx->print(F("short press = Overview"));
  gfx->setCursor(14, 76);
  gfx->print(F("hold = wake display"));

  drawPageDots(RGB565_DARKGREY);
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
  if (!stageCanvas) return;
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
  const int ys[4] = {2, 34, 66, 98};

  stageCanvas->fillScreen(RGB565_BLACK);
  stageCanvas->setTextSize(2);
  stageCanvas->setTextColor(RGB565_WHITE);
  for (int i = 0; i < 4; i++) {
    char row[28];
    snprintf(row, sizeof(row), "%s  %s%s", labels[i], lines[i], suffixes[i]);
    strncpy(lastOverview[i], row, sizeof(lastOverview[i]) - 1);
    lastOverview[i][sizeof(lastOverview[i]) - 1] = 0;
    stageCanvas->setCursor(4, ys[i]);
    stageCanvas->print(row);
  }
  stageCanvas->flush();
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

  if (!stageCanvas) return;

  const uint8_t size = fitTextSize(buf, SCREEN_WIDTH - 70, 9, 5);
  const int tw = textWidthPx(buf, size);
  const int th = 8 * size;
  const int x = (VALUE_STAGE_W - tw) / 2;
  const int y = (VALUE_STAGE_H - th) / 2;

  int ux = x + tw + 8;
  int uy = y + th - 18;
  const int unitWidth = textWidthPx(valueUnit, 2);
  if (ux + unitWidth > VALUE_STAGE_W - 4) {
    ux = x;
    uy = y + th + 4;
  }

  stageCanvas->fillScreen(RGB565_BLACK);
  stageCanvas->setTextSize(size);
  stageCanvas->setTextColor(RGB565_WHITE);
  stageCanvas->setCursor(x, y);
  stageCanvas->print(buf);

  // Unit to the right of the number
  stageCanvas->setTextSize(2);
  stageCanvas->setTextColor(valueAccent);
  stageCanvas->setCursor(ux, uy);
  stageCanvas->print(valueUnit);
  stageCanvas->flush();
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
  if (!stageCanvas) return;
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

  stageCanvas->fillScreen(RGB565_BLACK);
  stageCanvas->setTextSize(1);
  stageCanvas->setTextColor(RGB565_YELLOW);
  stageCanvas->setCursor(4, 2);
  stageCanvas->print(F("AVG"));

  const uint8_t size = fitTextSize(lines[0], SCREEN_WIDTH - 110, 7, 4);
  const int tw = textWidthPx(lines[0], size);
  const int th = 8 * size;
  const int x = 10;
  const int y = 18;
  stageCanvas->setTextSize(size);
  stageCanvas->setTextColor(RGB565_WHITE);
  stageCanvas->setCursor(x, y);
  stageCanvas->print(lines[0]);

  stageCanvas->setTextSize(2);
  stageCanvas->setTextColor(RGB565_YELLOW);
  stageCanvas->setCursor(x + tw + 8, y + th - 18);
  stageCanvas->print(F("L/100km"));

  strncpy(lastFuelLines[0], lines[0], sizeof(lastFuelLines[0]) - 1);
  lastFuelLines[0][sizeof(lastFuelLines[0]) - 1] = 0;
  strncpy(lastFuelLines[1], lines[1], sizeof(lastFuelLines[1]) - 1);
  lastFuelLines[1][sizeof(lastFuelLines[1]) - 1] = 0;
  strncpy(lastFuelLines[2], lines[2], sizeof(lastFuelLines[2]) - 1);
  lastFuelLines[2][sizeof(lastFuelLines[2]) - 1] = 0;

  stageCanvas->setTextSize(2);
  stageCanvas->setTextColor(RGB565_LIGHTGREY);
  stageCanvas->setCursor(4, 88);
  stageCanvas->print(lines[1]);
  stageCanvas->setCursor(4, 112);
  stageCanvas->print(lines[2]);
  stageCanvas->flush();
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
  if (!stageCanvas) return;
  char pctBuf[16];
  char litBuf[28];
  const float pct = telemetry.fuelLevelPct;
  const bool supported = elm.fuelLevelAvailable();

  if (!supported) {
    snprintf(pctBuf, sizeof(pctBuf), "N/A");
    snprintf(litBuf, sizeof(litBuf), "ECU has no fuel %% PID");
  } else if (isnan(pct)) {
    snprintf(pctBuf, sizeof(pctBuf), "--");
    snprintf(litBuf, sizeof(litBuf), "--.- L / %.0f L", TANK_CAPACITY_L);
  } else {
    snprintf(pctBuf, sizeof(pctBuf), "%.0f", pct);
    const float liters = TANK_CAPACITY_L * (pct / 100.0f);
    snprintf(litBuf, sizeof(litBuf), "%.1f L / %.0f L", liters, TANK_CAPACITY_L);
  }

  stageCanvas->fillScreen(RGB565_BLACK);

  const uint8_t size = fitTextSize(pctBuf, SCREEN_WIDTH - 80, 9, 5);
  const int tw = textWidthPx(pctBuf, size);
  const int th = 8 * size;
  const int x = (VALUE_STAGE_W - tw) / 2 - 10;
  const int y = (90 - th) / 2 + 6;
  stageCanvas->setTextSize(size);
  stageCanvas->setTextColor(RGB565_WHITE);
  stageCanvas->setCursor(x, y);
  stageCanvas->print(pctBuf);

  if (supported) {
    stageCanvas->setTextSize(3);
    stageCanvas->setTextColor(RGB565_GREENYELLOW);
    stageCanvas->setCursor(x + tw + 6, y + th - 28);
    stageCanvas->print('%');
  }

  strncpy(lastTankLines[0], pctBuf, sizeof(lastTankLines[0]) - 1);
  lastTankLines[0][sizeof(lastTankLines[0]) - 1] = 0;
  strncpy(lastTankLines[1], litBuf, sizeof(lastTankLines[1]) - 1);
  lastTankLines[1][sizeof(lastTankLines[1]) - 1] = 0;

  stageCanvas->setTextSize(2);
  stageCanvas->setTextColor(RGB565_LIGHTGREY);
  stageCanvas->setCursor(4, 100);
  stageCanvas->print(litBuf);
  stageCanvas->flush();
}

void renderLive() {
  const bool needChrome = layoutDirty || drawnPage != (int8_t)page;

  if (needChrome) {
    switch (page) {
      case PAGE_OVERVIEW: drawOverviewChrome(); break;
      case PAGE_BAT: drawValueChrome("BAT LVL", "V", RGB565_GREEN); break;
      case PAGE_RPM: drawValueChrome("RPM", "rpm", RGB565_ORANGE); break;
      case PAGE_COOLANT: drawValueChrome("Coolant", "C", RGB565_RED); break;
      case PAGE_AMBIENT: drawValueChrome("Intake", "C", RGB565_CYAN); break;
      case PAGE_TANK: drawTankChrome(); break;
      case PAGE_TRIP: drawFuelChrome(); break;
      case PAGE_SCREEN_OFF: drawScreenOffChrome(); break;
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
    case PAGE_RPM: drawBigValue(rpmDisplay, 0); break;
    case PAGE_COOLANT: drawBigValue(telemetry.coolantC, 0); break;
    case PAGE_AMBIENT: drawBigValue(telemetry.intakeC, 0); break;
    case PAGE_TANK: drawTankValues(); break;
    case PAGE_TRIP: drawFuelValues(); break;
    case PAGE_SCREEN_OFF: break;
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
  telemetry = ObdData{};  // start clean; no stale mock/live mix
  resetRpmDisplay();
  restoreUiState();
  showStatus("Connected!", "restoring last page");
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
  setCpuFrequencyMhz(CPU_FREQ_MHZ);
  Serial.printf("\n=== ESP32-C6 BLE OBD LCD 1.47 ===\n");
  Serial.printf("[SYS] CPU %u MHz  FW %s\n",
                (unsigned)getCpuFrequencyMhz(), FIRMWARE_VERSION);

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
  uiPrefs.begin("ui", false);

  if (!gfx->begin()) {
    Serial.println("LCD init failed");
  }
  if (!stageCanvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    Serial.println("Value canvas alloc failed");
  }
  gfx->fillScreen(RGB565_BLACK);

  BleElmClient::setYieldCallback(elmUiYield);

  // Delay BLE init until we know mock isn't requested (mock = radio off).
  showStatus("BOOT x5 = MOCK", "hold Overview=OTA");
  const unsigned long waitStart = millis();
  while (millis() - waitStart < MOCK_BOOT_WAIT_MS) {
    bool pe = false, re = false, lp = false;
    pollButton(pe, re, lp);
    if (pe && handleRapidTapForMock()) break;
    delay(10);
  }

  if (uiState != UI_MOCK) {
    bleObd.begin("ESP32-OBD-LCD");
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
    } else if (releasedEdge && !btnLongHandled &&
               (millis() - btnPressStartMs) < BTN_LONG_MS) {
      applyUiState(PAGE_OVERVIEW, false);
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

  if (pendingLongPress) {
    pendingLongPress = false;
    handleLongPressActions();
  }

  if (pendingNextPage) {
    pendingNextPage = false;
    if (uiState == UI_LIVE || uiState == UI_MOCK) {
      nextPage();
    }
  }

  if (longPress) {
    handleLongPressActions();
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
      if (!bleObd.isStarted()) {
        bleObd.begin("ESP32-OBD-LCD");
      }
      bleObd.disconnect();
      connectAndInit();
    }
    delay(20);
    return;
  } else if (millis() - lastPidMs >= PID_INTERVAL_MS) {
    lastPidMs = millis();
    // Prefer queued page flips over starting another blocking PID.
    if (!pendingNextPage && !pendingLongPress) {
      if (elm.pollForPage(telemetry, page)) {
        valuesDirty = true;
      }
    }
  }

  if (liveLike && (millis() - lastTripMs >= 200)) {
    lastTripMs = millis();
    trip.update(telemetry, uiState == UI_MOCK);
    if (page == PAGE_OVERVIEW || page == PAGE_TANK || page == PAGE_TRIP) {
      valuesDirty = true;
    }
  }

  if (liveLike && page == PAGE_RPM && !displayOff) {
    updateRpmDisplay();
  }

  if (liveLike && (layoutDirty || valuesDirty)) {
    renderLive();
  } else if (liveLike && page == PAGE_RPM && !displayOff) {
    const int prev = lastRpmShown;
    const int cur = isnan(rpmDisplay) ? -1 : (int)(rpmDisplay + 0.5f);
    if (cur != prev) {
      lastRpmShown = cur;
      drawBigValue(rpmDisplay, 0);
    }
  }

  delay(10);
}
