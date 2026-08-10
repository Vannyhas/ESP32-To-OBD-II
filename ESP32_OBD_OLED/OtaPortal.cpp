#include "OtaPortal.h"
#include "config.h"

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>

namespace OtaPortal {
namespace {

WebServer server(80);
bool active = false;

const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>ESP32 OBD OTA</title>
<style>
 body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:24px}
 .card{max-width:420px;margin:auto;padding:20px;border:1px solid #333;border-radius:12px;background:#1a1a1a}
 h1{font-size:1.2rem;margin:0 0 12px}
 p{opacity:.8;font-size:.9rem}
 input[type=file]{width:100%;margin:12px 0}
 button{width:100%;padding:12px;border:0;border-radius:8px;background:#0a7;color:#fff;font-weight:700}
 .ok{color:#6f6}.err{color:#f66}
</style>
</head>
<body>
<div class="card">
 <h1>ESP32-OBD Firmware Update</h1>
 <p>Select firmware <b>.bin</b> from Arduino build and upload.</p>
 <form method="POST" action="/update" enctype="multipart/form-data">
  <input type="file" name="firmware" accept=".bin" required>
  <button type="submit">Upload &amp; flash</button>
 </form>
 <p id="msg"></p>
</div>
</body>
</html>
)HTML";

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleUpdateFinish() {
  if (Update.hasError()) {
    server.send(500, "text/plain", "Update failed");
  } else {
    server.send(200, "text/plain", "OK - rebooting");
    delay(400);
    ESP.restart();
  }
}

void handleUpdateWrite() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] Start: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[OTA] Success: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

}  // namespace

bool begin() {
  if (active) return true;

  WiFi.mode(WIFI_AP);
  const bool ok = WiFi.softAP(OTA_AP_SSID, OTA_AP_PASS);
  delay(120);

  server.on("/", HTTP_GET, handleRoot);
  server.on(
      "/update", HTTP_POST,
      handleUpdateFinish,
      handleUpdateWrite);
  server.begin();

  active = ok;
  Serial.printf("[OTA] AP %s  IP %s  ok=%d\n",
                OTA_AP_SSID,
                WiFi.softAPIP().toString().c_str(),
                (int)ok);
  return ok;
}

void loop() {
  if (!active) return;
  server.handleClient();
}

void stop() {
  if (!active) return;
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  active = false;
  Serial.println("[OTA] stopped");
}

bool isActive() { return active; }
const char* ssid() { return OTA_AP_SSID; }
const char* password() { return OTA_AP_PASS; }
String apIp() { return WiFi.softAPIP().toString(); }

}  // namespace OtaPortal
