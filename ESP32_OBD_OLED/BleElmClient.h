#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "config.h"

// Stream-like BLE UART client for ELM327 adapters.
class BleElmClient : public NimBLEClientCallbacks {
 public:
  bool begin(const char* localName = "ESP32-OBD");
  void end();  // disconnect + NimBLE deinit (cool down radio)
  bool isStarted() const { return started_; }
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

  // Called while waiting for ELM '>' so UI can service the button.
  using YieldFn = void (*)();
  static void setYieldCallback(YieldFn fn) { yieldFn_ = fn; }

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
  bool started_ = false;
  int activePreset_ = -1;

  static YieldFn yieldFn_;
};

extern BleElmClient bleObd;
