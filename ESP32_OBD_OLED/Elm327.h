#pragma once

#include "BleElmClient.h"

struct ObdData {
  float voltage = NAN;      // AT RV / PID 0142
  float rpm = NAN;          // 010C
  float coolantC = NAN;     // 0105
  float speedKmh = NAN;     // 010D
  float throttlePct = NAN;  // 0111
  float engineLoad = NAN;   // 0104
  float mafGps = NAN;       // 0110 g/s
  float fuelRateLph = NAN;  // 015E L/h (if ECU supports)
  float fuelLevelPct = NAN; // 012F tank level %
  float ambientC = NAN;     // 0146 outside air °C
  bool valid = false;
};

class Elm327 {
 public:
  bool begin(BleElmClient& link);
  bool isReady() const { return ready_; }

  bool queryVoltage(float& out);
  bool queryRpm(float& out);
  bool queryCoolantC(float& out);
  bool querySpeed(float& out);
  bool queryThrottle(float& out);
  bool queryEngineLoad(float& out);
  bool queryMaf(float& out);
  bool queryFuelRate(float& out);
  bool queryFuelLevel(float& out);
  bool queryAmbientC(float& out);

  bool pollNext(ObdData& data);  // round-robin one PID per call
  // Prefer PIDs for the active UI page; still refreshes trip sensors.
  bool pollForPage(ObdData& data, uint8_t page);

 private:
  bool sendAt(const char* cmd, String& response);
  bool sendPid(uint8_t mode, uint8_t pid, uint8_t* payload, size_t& payloadLen);
  static int hexByte(const String& s, int idx);

  BleElmClient* link_ = nullptr;
  bool ready_ = false;
  uint8_t nextPid_ = 0;
  bool fuelRateSupported_ = true;  // disable after first NO DATA
  bool fuelLevelSupported_ = true;
  bool ambientSupported_ = true;
};
