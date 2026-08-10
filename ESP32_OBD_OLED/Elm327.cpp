#include "Elm327.h"
#include "config.h"
#include <stdio.h>

bool Elm327::begin(BleElmClient& link) {
  link_ = &link;
  ready_ = false;
  fuelRateSupported_ = true;
  fuelLevelSupported_ = true;
  ambientSupported_ = true;
  fuelLevelFailStreak_ = 0;
  ambientFailStreak_ = 0;
  if (!link_->isConnected()) return false;

  delay(300);
  link_->flushInput();

  // Fixed ISO 9141-2 for Toyota Harrier 1999 (not auto-detect).
  char spCmd[8];
  snprintf(spCmd, sizeof(spCmd), "ATSP%s", OBD_ELM_PROTOCOL);

  const char* initCmds[] = {
      "ATZ",   // reset
      "ATE0",  // echo off
      "ATL0",  // no linefeeds
      "ATS0",  // no spaces
      "ATH0",  // headers off
      "ATAT1", // adaptive timing (helps ISO 9141)
      spCmd,   // ATSP3 = ISO 9141-2
  };

  for (const char* cmd : initCmds) {
    String resp;
    if (!sendAt(cmd, resp)) {
      Serial.printf("[ELM] init fail on %s\n", cmd);
      return false;
    }
    Serial.printf("[ELM] %s -> %s\n", cmd, resp.c_str());
    if (strcmp(cmd, "ATZ") == 0) delay(1000);
    if (strncmp(cmd, "ATSP", 4) == 0) delay(300);
  }

  // ISO 9141-2 does 5-baud init on first request — give it time
  Serial.println("[ELM] ISO 9141-2 handshake (may take a few seconds)...");
  float dummy = NAN;
  const unsigned long t0 = millis();
  bool linked = false;
  while (millis() - t0 < 8000) {
    if (queryRpm(dummy) || queryVoltage(dummy)) {
      linked = true;
      break;
    }
    delay(200);
  }
  if (!linked) {
    Serial.println("[ELM] ISO 9141-2 handshake failed");
    return false;
  }
  Serial.printf("[ELM] protocol locked ATSP%s (ISO 9141-2)\n", OBD_ELM_PROTOCOL);

  ready_ = true;
  return true;
}

bool Elm327::sendAt(const char* cmd, String& response) {
  if (!link_ || !link_->isConnected()) return false;
  link_->flushInput();
  link_->println(cmd);

  response = link_->readStringUntil('>');
  response.trim();
  response.replace("\r", "");
  response.replace("\n", "");
  return response.length() > 0 && response.indexOf("?") < 0;
}

int Elm327::hexByte(const String& s, int idx) {
  if (idx + 1 >= (int)s.length()) return -1;
  char buf[3] = {s[idx], s[idx + 1], 0};
  return (int)strtol(buf, nullptr, 16);
}

bool Elm327::sendPid(uint8_t mode, uint8_t pid, uint8_t* payload,
                     size_t& payloadLen) {
  if (!link_ || !link_->isConnected()) return false;

  char cmd[8];
  snprintf(cmd, sizeof(cmd), "%02X%02X", mode, pid);

  link_->flushInput();
  link_->println(cmd);

  String raw = link_->readStringUntil('>');
  raw.toUpperCase();
  raw.replace(" ", "");
  raw.replace("\r", "");
  raw.replace("\n", "");
  raw.replace("SEARCHING...", "");

  if (raw.indexOf("NO DATA") >= 0 || raw.indexOf("UNABLE") >= 0 ||
      raw.indexOf("ERROR") >= 0 || raw.indexOf("?") >= 0) {
    Serial.printf("[ELM] PID %s fail: %s\n", cmd, raw.c_str());
    return false;
  }

  // Expect response like 41XXDD... (mode+0x40)
  char expect[5];
  snprintf(expect, sizeof(expect), "%02X%02X", mode + 0x40, pid);
  int pos = raw.indexOf(expect);
  if (pos < 0) {
    Serial.printf("[ELM] PID %s bad resp: %s\n", cmd, raw.c_str());
    return false;
  }

  int dataStart = pos + 4;
  payloadLen = 0;
  while (dataStart + 1 < (int)raw.length() && payloadLen < 8) {
    int b = hexByte(raw, dataStart);
    if (b < 0) break;
    payload[payloadLen++] = (uint8_t)b;
    dataStart += 2;
  }
  return payloadLen > 0;
}

bool Elm327::queryVoltage(float& out) {
  // Prefer AT RV (adapter supply ≈ battery with ignition on)
  String resp;
  if (sendAt("ATRV", resp)) {
    // e.g. "12.6V" or "12.6"
    resp.replace("V", "");
    resp.trim();
    float v = resp.toFloat();
    if (v > 5.0f && v < 20.0f) {
      out = v;
      return true;
    }
  }

  // Fallback: PID 0142 Control module voltage
  uint8_t data[8];
  size_t n = 0;
  if (!sendPid(0x01, 0x42, data, n) || n < 2) return false;
  out = ((data[0] * 256.0f) + data[1]) / 1000.0f;
  return true;
}

bool Elm327::queryRpm(float& out) {
  uint8_t data[8];
  size_t n = 0;
  if (!sendPid(0x01, 0x0C, data, n) || n < 2) return false;
  out = ((data[0] * 256.0f) + data[1]) / 4.0f;
  return true;
}

bool Elm327::queryCoolantC(float& out) {
  uint8_t data[8];
  size_t n = 0;
  if (!sendPid(0x01, 0x05, data, n) || n < 1) return false;
  out = (float)data[0] - 40.0f;
  return true;
}

bool Elm327::querySpeed(float& out) {
  uint8_t data[8];
  size_t n = 0;
  if (!sendPid(0x01, 0x0D, data, n) || n < 1) return false;
  out = (float)data[0];
  return true;
}

bool Elm327::queryThrottle(float& out) {
  uint8_t data[8];
  size_t n = 0;
  if (!sendPid(0x01, 0x11, data, n) || n < 1) return false;
  out = data[0] * 100.0f / 255.0f;
  return true;
}

bool Elm327::queryEngineLoad(float& out) {
  uint8_t data[8];
  size_t n = 0;
  if (!sendPid(0x01, 0x04, data, n) || n < 1) return false;
  out = data[0] * 100.0f / 255.0f;
  return true;
}

bool Elm327::queryMaf(float& out) {
  uint8_t data[8];
  size_t n = 0;
  if (!sendPid(0x01, 0x10, data, n) || n < 2) return false;
  out = ((data[0] * 256.0f) + data[1]) / 100.0f;  // g/s
  return true;
}

bool Elm327::queryFuelRate(float& out) {
  uint8_t data[8];
  size_t n = 0;
  if (!sendPid(0x01, 0x5E, data, n) || n < 2) return false;
  out = ((data[0] * 256.0f) + data[1]) / 20.0f;  // L/h
  return true;
}

bool Elm327::queryFuelLevel(float& out) {
  uint8_t data[8];
  size_t n = 0;
  if (!sendPid(0x01, 0x2F, data, n) || n < 1) return false;
  out = data[0] * 100.0f / 255.0f;  // %
  return true;
}

bool Elm327::queryAmbientC(float& out) {
  uint8_t data[8];
  size_t n = 0;
  if (!sendPid(0x01, 0x46, data, n) || n < 1) return false;
  out = (float)data[0] - 40.0f;
  return true;
}

bool Elm327::queryIntakeC(float& out) {
  uint8_t data[8];
  size_t n = 0;
  if (!sendPid(0x01, 0x0F, data, n) || n < 1) return false;
  out = (float)data[0] - 40.0f;  // intake air temp
  return true;
}

bool Elm327::pollNext(ObdData& data) {
  return pollForPage(data, 255);  // no focus
}

bool Elm327::pollForPage(ObdData& data, uint8_t page) {
  if (!ready_) return false;

  // Keep trip math alive: every 3rd request is speed or MAF/fuel-rate.
  static uint8_t tick = 0;
  tick++;

  bool ok = false;

  if ((tick % 3) == 0) {
    if ((tick % 6) == 0) {
      ok = querySpeed(data.speedKmh);
    } else if (fuelRateSupported_) {
      ok = queryFuelRate(data.fuelRateLph);
      if (!ok) {
        fuelRateSupported_ = false;
        ok = queryMaf(data.mafGps);
      }
    } else {
      ok = queryMaf(data.mafGps);
    }
    if (ok) data.valid = true;
    return ok;
  }

  switch (page) {
    case 0: {  // PAGE_OVERVIEW — rotate visible fields
      static uint8_t ov = 0;
      switch (ov++ % 3) {
        case 0: ok = queryVoltage(data.voltage); break;
        case 1: ok = queryCoolantC(data.coolantC); break;
        default: ok = queryEngineLoad(data.engineLoad); break;
      }
      break;
    }
    case 1:  // PAGE_BAT
      ok = queryVoltage(data.voltage);
      break;
    case 2:  // PAGE_RPM
      ok = queryRpm(data.rpm);
      break;
    case 3:  // PAGE_COOLANT
      ok = queryCoolantC(data.coolantC);
      break;
    case 4:  // PAGE_AMBIENT → Intake Air Temp PID 010F (Torque "Intake")
      ok = queryIntakeC(data.intakeC);
      break;
    case 5:  // PAGE_TANK — 012F often missing on pre‑CAN Toyota
      if (fuelLevelSupported_) {
        ok = queryFuelLevel(data.fuelLevelPct);
        if (ok) {
          fuelLevelFailStreak_ = 0;
        } else if (++fuelLevelFailStreak_ >= 3) {
          fuelLevelSupported_ = false;
          Serial.println("[ELM] 012F fuel level unsupported by ECU");
        }
      } else {
        ok = false;
      }
      break;
    case 6:  // PAGE_TRIP — already doing speed/maf often
      ok = querySpeed(data.speedKmh);
      break;
    default: {
      // background round-robin for unused PIDs
      static uint8_t bg = 0;
      switch (bg++ % 4) {
        case 0: ok = queryVoltage(data.voltage); break;
        case 1: ok = queryCoolantC(data.coolantC); break;
        case 2: ok = queryEngineLoad(data.engineLoad); break;
        default: ok = queryRpm(data.rpm); break;
      }
      break;
    }
  }

  if (ok) data.valid = true;
  return ok;
}
