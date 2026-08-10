#include "OtaPull.h"
#include "config.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <stdlib.h>
#include <string.h>

namespace OtaPull {
namespace {

void report(StatusFn fn, Status st, const char* detail, int pct = -1) {
  if (fn) fn(st, detail ? detail : "", pct);
}

// Compare simple semver "X.Y.Z" (missing parts = 0). Returns <0, 0, >0.
int cmpVersion(const char* a, const char* b) {
  int a1 = 0, a2 = 0, a3 = 0;
  int b1 = 0, b2 = 0, b3 = 0;
  sscanf(a, "%d.%d.%d", &a1, &a2, &a3);
  sscanf(b, "%d.%d.%d", &b1, &b2, &b3);
  if (a1 != b1) return a1 - b1;
  if (a2 != b2) return a2 - b2;
  return a3 - b3;
}

bool extractJsonString(const String& json, const char* key, String& out) {
  // Minimal extractor: "key" : "value"
  String needle = String("\"") + key + "\"";
  int k = json.indexOf(needle);
  if (k < 0) return false;
  int colon = json.indexOf(':', k + needle.length());
  if (colon < 0) return false;
  int q1 = json.indexOf('"', colon + 1);
  if (q1 < 0) return false;
  int q2 = json.indexOf('"', q1 + 1);
  if (q2 < 0) return false;
  out = json.substring(q1 + 1, q2);
  return out.length() > 0;
}

bool ssidEquals(const String& a, const char* b) {
  if (!b) return false;
  String aa = a;
  aa.trim();
  String bb(b);
  bb.trim();
  return aa.equalsIgnoreCase(bb);
}

bool connectWifi(StatusFn onStatus) {
  report(onStatus, Status::ConnectingWifi, OTA_WIFI_SSID);

  // After NimBLE, radio needs a clean handoff to Wi‑Fi STA.
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(800);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
#if defined(WIFI_AUTH_WPA_PSK)
  // Prefer widest compatibility with phone hotspots (WPA2 / mixed).
  WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
#endif
  delay(300);

  // Scan 2.4 GHz only (ESP32-C6 has no 5 GHz). Xiaomi hotspot on 5 GHz = invisible.
  report(onStatus, Status::ConnectingWifi, "Scanning 2.4G...");
  int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
  if (n < 0) {
    delay(500);
    n = WiFi.scanNetworks(false, true);
  }

  bool seen = false;
  int rssi = 0;
  Serial.printf("[OTA] scan n=%d looking for '%s'\n", n, OTA_WIFI_SSID);
  for (int i = 0; i < n; i++) {
    const String id = WiFi.SSID(i);
    Serial.printf("[OTA]  [%d] '%s' %d dBm ch=%d\n",
                  i, id.c_str(), WiFi.RSSI(i), WiFi.channel(i));
    if (ssidEquals(id, OTA_WIFI_SSID)) {
      seen = true;
      rssi = WiFi.RSSI(i);
    }
  }

  if (!seen) {
    // Show what we actually hear — helps catch 5 GHz / wrong name.
    char line[48];
    if (n <= 0) {
      snprintf(line, sizeof(line), "0 nets (need 2.4G)");
    } else {
      snprintf(line, sizeof(line), "no %s (%d nets)", OTA_WIFI_SSID, n);
    }
    report(onStatus, Status::WifiFailed, line);
    // Keep a few SSIDs on screen for ~2.5s via detail + Serial above.
    delay(2500);

    // Still try begin() — rare cases scan misses an AP that can associate.
    report(onStatus, Status::ConnectingWifi, "Trying blind...");
  } else {
    char joining[40];
    snprintf(joining, sizeof(joining), "%s %ddBm", OTA_WIFI_SSID, rssi);
    report(onStatus, Status::ConnectingWifi, joining);
  }

  WiFi.scanDelete();
  WiFi.begin(OTA_WIFI_SSID, OTA_WIFI_PASS);

  const unsigned long t0 = millis();
  wl_status_t last = WL_IDLE_STATUS;
  while (WiFi.status() != WL_CONNECTED) {
    const wl_status_t st = WiFi.status();
    if (st != last) {
      last = st;
      Serial.printf("[OTA] WiFi status=%d\n", (int)st);
    }
    if (millis() - t0 > OTA_WIFI_TIMEOUT_MS) {
      char fail[40];
      if (!seen) {
        snprintf(fail, sizeof(fail), "Set hotspot 2.4GHz");
      } else {
        snprintf(fail, sizeof(fail), "Auth? st=%d", (int)st);
      }
      report(onStatus, Status::WifiFailed, fail);
      WiFi.disconnect(true, true);
      WiFi.mode(WIFI_OFF);
      return false;
    }
    delay(250);
  }

  Serial.printf("[OTA] WiFi OK %s\n", WiFi.localIP().toString().c_str());
  report(onStatus, Status::ConnectingWifi, WiFi.localIP().toString().c_str());
  return true;
}

bool httpGet(const String& url, String& body, StatusFn onStatus, Status failSt) {
  WiFiClientSecure client;
  client.setInsecure();  // hobby: trust GitHub TLS without bundling CA

  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("ESP32-OBD-OTA/1.0");
  if (!http.begin(client, url)) {
    report(onStatus, failSt, "HTTP begin fail");
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char buf[32];
    snprintf(buf, sizeof(buf), "HTTP %d", code);
    report(onStatus, failSt, buf);
    http.end();
    return false;
  }
  body = http.getString();
  http.end();
  return true;
}

bool downloadAndFlash(const String& url, StatusFn onStatus) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(60000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("ESP32-OBD-OTA/1.0");
  if (!http.begin(client, url)) {
    report(onStatus, Status::FlashFailed, "HTTP begin fail");
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char buf[32];
    snprintf(buf, sizeof(buf), "HTTP %d", code);
    report(onStatus, Status::FlashFailed, buf);
    http.end();
    return false;
  }

  const int contentLen = http.getSize();
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    report(onStatus, Status::FlashFailed, "No stream");
    http.end();
    return false;
  }

  if (!Update.begin(contentLen > 0 ? (size_t)contentLen : UPDATE_SIZE_UNKNOWN)) {
    report(onStatus, Status::FlashFailed, "Update.begin");
    http.end();
    return false;
  }

  report(onStatus, Status::Downloading, "0%", 0);
  uint8_t buf[1024];
  size_t written = 0;
  int lastPct = -1;
  const unsigned long t0 = millis();

  while (http.connected() && (contentLen < 0 || (int)written < contentLen)) {
    const size_t avail = stream->available();
    if (!avail) {
      if (millis() - t0 > 120000UL) {
        Update.abort();
        report(onStatus, Status::FlashFailed, "Timeout");
        http.end();
        return false;
      }
      delay(1);
      continue;
    }
    const size_t n = stream->readBytes(buf, min(avail, sizeof(buf)));
    if (n == 0) continue;
    if (Update.write(buf, n) != n) {
      Update.abort();
      report(onStatus, Status::FlashFailed, "Write err");
      http.end();
      return false;
    }
    written += n;
    if (contentLen > 0) {
      const int pct = (int)((written * 100UL) / (size_t)contentLen);
      if (pct != lastPct && (pct % 5 == 0 || pct == 100)) {
        lastPct = pct;
        char detail[16];
        snprintf(detail, sizeof(detail), "%d%%", pct);
        report(onStatus, Status::Downloading, detail, pct);
      }
    }
  }

  if (!Update.end(true)) {
    report(onStatus, Status::FlashFailed, "Update.end");
    http.end();
    return false;
  }

  http.end();
  Serial.printf("[OTA] flashed %u bytes\n", (unsigned)written);
  return true;
}

}  // namespace

const char* firmwareVersion() { return FIRMWARE_VERSION; }

bool checkAndUpdate(StatusFn onStatus) {
  if (!connectWifi(onStatus)) return false;

  report(onStatus, Status::FetchingManifest, "manifest.json");
  String manifestUrl = String(OTA_GITHUB_BASE) + "manifest.json";
  String json;
  if (!httpGet(manifestUrl, json, onStatus, Status::ManifestFailed)) {
    WiFi.mode(WIFI_OFF);
    return false;
  }

  String remoteVer;
  String fileRel;
  if (!extractJsonString(json, "version", remoteVer) ||
      !extractJsonString(json, "file", fileRel)) {
    report(onStatus, Status::ManifestFailed, "Bad manifest");
    WiFi.mode(WIFI_OFF);
    return false;
  }

  Serial.printf("[OTA] local=%s remote=%s file=%s\n",
                FIRMWARE_VERSION, remoteVer.c_str(), fileRel.c_str());

  if (cmpVersion(remoteVer.c_str(), FIRMWARE_VERSION) <= 0) {
    report(onStatus, Status::UpToDate, remoteVer.c_str());
    WiFi.mode(WIFI_OFF);
    return false;
  }

  String binUrl = String(OTA_GITHUB_BASE) + fileRel;
  char msg[48];
  snprintf(msg, sizeof(msg), "v%s", remoteVer.c_str());
  report(onStatus, Status::Downloading, msg, 0);

  if (!downloadAndFlash(binUrl, onStatus)) {
    WiFi.mode(WIFI_OFF);
    return false;
  }

  report(onStatus, Status::Rebooting, remoteVer.c_str());
  delay(500);
  ESP.restart();
  return true;  // not reached
}

}  // namespace OtaPull
