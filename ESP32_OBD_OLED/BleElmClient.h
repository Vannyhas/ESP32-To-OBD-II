#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "config.h"

// Stream-like BLE UART client for ELM327 adapters.
class BleElmClient : public NimBLEClientCallbacks {
 public:
  bool begin(const char* localName = "ESP32-OBD");
  bool connectToObd(const String& target = OBD_TARGET_NAME);
  void disconnect();
  bool isConnected() const;

  size_t write(const uint8_t* data, size_t len);
  size_t print(const char* s);
  size_t println(const char* s);

  int available();
  int read();
  String readStringUntil(char terminator);
  void flushInput();

  // NimBLEClientCallbacks
  void onDisconnect(NimBLEClient* client, int reason) override;
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override;
  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) override;
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;

 private:
  bool tryConnect(const NimBLEAddress& addr, int preset);
  bool subscribeNotify(NimBLERemoteCharacteristic* rx);

  NimBLEClient* client_ = nullptr;
  NimBLERemoteCharacteristic* tx_ = nullptr;  // write to adapter
  NimBLERemoteCharacteristic* rx_ = nullptr;  // notify from adapter

  String rxBuf_;
  volatile bool connected_ = false;
  int activePreset_ = -1;
};

extern BleElmClient bleObd;
