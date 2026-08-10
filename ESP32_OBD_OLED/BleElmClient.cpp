#include "BleElmClient.h"

BleElmClient bleObd;

namespace {

struct UuidSet {
  const char* service;
  const char* notifyChar;  // RX from adapter
  const char* writeChar;   // TX to adapter
  const char* label;
};

const UuidSet kPresets[UUID_PRESET_COUNT] = {
  {"FFF0", "FFF1", "FFF2", "FFF0"},
  {"18F0", "2AF0", "2AF1", "18F0"},
  {"6E400001-B5A3-F393-E0A9-E50E24DCCA9E",
   "6E400003-B5A3-F393-E0A9-E50E24DCCA9E",
   "6E400002-B5A3-F393-E0A9-E50E24DCCA9E",
   "NordicUART"},
};

bool nameLooksLikeObd(const std::string& name) {
  String n = String(name.c_str());
  n.toUpperCase();
  return n.indexOf("OBD") >= 0 || n.indexOf("ELM") >= 0 ||
         n.indexOf("VGATE") >= 0 || n.indexOf("VEEPEAK") >= 0 ||
         n.indexOf("CARISTA") >= 0 || n.indexOf("LELINK") >= 0 ||
         n.indexOf("KONNWEI") >= 0 || n.indexOf("OBDBLE") >= 0;
}

bool targetMatches(const NimBLEAdvertisedDevice* adv, const String& target) {
  if (target.length() == 0) {
    return nameLooksLikeObd(adv->getName());
  }

  String t = target;
  t.trim();

  // MAC form AA:BB:CC:DD:EE:FF
  if (t.indexOf(':') > 0 && t.length() >= 17) {
    String addr = String(adv->getAddress().toString().c_str());
    return addr.equalsIgnoreCase(t);
  }

  String name = String(adv->getName().c_str());
  return name.equalsIgnoreCase(t) || name.indexOf(t) >= 0;
}

}  // namespace

bool BleElmClient::begin(const char* localName) {
  NimBLEDevice::init(localName);
  NimBLEDevice::setPower(9);  // dBm, NimBLE 2.x

  // Bonding + passkey available if the adapter asks for it.
  // MITM off by default so "Just Works" adapters still connect.
  NimBLEDevice::setSecurityAuth(true /* bond */, false /* MITM */, false /* SC */);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_DISPLAY);
  NimBLEDevice::setSecurityPasskey(OBD_BLE_PIN);
  NimBLEDevice::deleteAllBonds();
  Serial.printf("[BLE] Ready, PIN=%u\n", (unsigned)OBD_BLE_PIN);
  return true;
}

bool BleElmClient::connectToObd(const String& target) {
  disconnect();

  Serial.printf("[BLE] Scanning %d s for OBD...\n", BLE_SCAN_SECONDS);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(15);

  NimBLEScanResults results = scan->getResults(BLE_SCAN_SECONDS * 1000, false);

  bool haveTarget = false;
  NimBLEAddress targetAddr;
  std::string targetName;

  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* adv = results.getDevice(i);
    if (!adv) continue;

    Serial.printf("  [%d] %s type=%u  %s  RSSI=%d\n", i,
                  adv->getAddress().toString().c_str(),
                  (unsigned)adv->getAddressType(),
                  adv->getName().c_str(),
                  adv->getRSSI());

    if (!haveTarget && targetMatches(adv, target)) {
      targetAddr = adv->getAddress();
      targetName = adv->getName();
      haveTarget = true;
    }
  }
  scan->clearResults();

  if (!haveTarget) {
    Serial.println("[BLE] No matching OBD adapter found");
    return false;
  }

  Serial.printf("[BLE] Connecting to %s (%s)\n",
                targetName.c_str(),
                targetAddr.toString().c_str());

  bool ok = false;
  for (int p = 0; p < UUID_PRESET_COUNT && !ok; p++) {
    ok = tryConnect(targetAddr, p);
  }
  return ok;
}

bool BleElmClient::tryConnect(const NimBLEAddress& addr, int preset) {
  const UuidSet& u = kPresets[preset];
  Serial.printf("[BLE] Trying UUID preset %s...\n", u.label);

  if (client_) {
    if (client_->isConnected()) client_->disconnect();
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
  }

  client_ = NimBLEDevice::createClient();
  client_->setClientCallbacks(this, false);
  client_->setConnectTimeout(10 * 1000);  // ms in NimBLE 2.x

  if (!client_->connect(addr)) {
    Serial.println("[BLE] connect() failed");
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
    return false;
  }

#if OBD_FORCE_PAIR
  Serial.printf("[BLE] Pairing with PIN %u...\n", (unsigned)OBD_BLE_PIN);
  NimBLEDevice::setSecurityAuth(true, true, false);
  if (!client_->secureConnection()) {
    Serial.println("[BLE] secureConnection() failed (continuing anyway)");
  }
#endif

  NimBLERemoteService* svc = client_->getService(u.service);
  if (!svc) {
    // Some adapters expose services only after pairing
    Serial.println("[BLE] Service missing — trying pair with PIN...");
    NimBLEDevice::setSecurityAuth(true, true, false);
    if (client_->secureConnection()) {
      delay(200);
      svc = client_->getService(u.service);
    }
  }
  if (!svc) {
    Serial.printf("[BLE] Service %s not found\n", u.service);
    client_->disconnect();
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
    return false;
  }

  rx_ = svc->getCharacteristic(u.notifyChar);
  tx_ = svc->getCharacteristic(u.writeChar);

  if (!rx_ || !tx_) {
    Serial.println("[BLE] TX/RX characteristics missing");
    client_->disconnect();
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
    rx_ = nullptr;
    tx_ = nullptr;
    return false;
  }

  if (!subscribeNotify(rx_)) {
    Serial.println("[BLE] Notify subscribe failed");
    client_->disconnect();
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
    rx_ = nullptr;
    tx_ = nullptr;
    return false;
  }

  connected_ = true;
  activePreset_ = preset;
  Serial.printf("[BLE] Connected via %s\n", u.label);
  return true;
}

bool BleElmClient::subscribeNotify(NimBLERemoteCharacteristic* rx) {
  if (!rx->canNotify() && !rx->canIndicate()) {
    return false;
  }

  return rx->subscribe(
      true,
      [this](NimBLERemoteCharacteristic* /*c*/, uint8_t* data, size_t len,
             bool /*isNotify*/) {
        for (size_t i = 0; i < len; i++) {
          rxBuf_ += (char)data[i];
        }
      });
}

void BleElmClient::disconnect() {
  if (client_) {
    if (client_->isConnected()) client_->disconnect();
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
  }
  tx_ = nullptr;
  rx_ = nullptr;
  connected_ = false;
  rxBuf_ = "";
}

bool BleElmClient::isConnected() const {
  return connected_ && client_ && client_->isConnected();
}

void BleElmClient::onDisconnect(NimBLEClient* /*client*/, int reason) {
  Serial.printf("[BLE] Disconnected, reason=%d\n", reason);
  connected_ = false;
  tx_ = nullptr;
  rx_ = nullptr;
}

void BleElmClient::onPassKeyEntry(NimBLEConnInfo& connInfo) {
  Serial.printf("[BLE] Passkey entry -> %u\n", (unsigned)OBD_BLE_PIN);
  NimBLEDevice::injectPassKey(connInfo, OBD_BLE_PIN);
}

void BleElmClient::onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) {
  Serial.printf("[BLE] Confirm passkey %u\n", (unsigned)pin);
  NimBLEDevice::injectConfirmPasskey(connInfo, true);
}

void BleElmClient::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
  Serial.printf("[BLE] Auth complete, encrypted=%d bonded=%d\n",
                connInfo.isEncrypted(), connInfo.isBonded());
}

size_t BleElmClient::write(const uint8_t* data, size_t len) {
  if (!isConnected() || !tx_) return 0;

  bool ok = false;
  if (tx_->canWrite()) {
    ok = tx_->writeValue(data, len, false);
  } else if (tx_->canWriteNoResponse()) {
    ok = tx_->writeValue(data, len, false);
  }
  return ok ? len : 0;
}

size_t BleElmClient::print(const char* s) {
  return write((const uint8_t*)s, strlen(s));
}

size_t BleElmClient::println(const char* s) {
  size_t n = print(s);
  n += write((const uint8_t*)"\r", 1);
  return n;
}

int BleElmClient::available() { return rxBuf_.length(); }

int BleElmClient::read() {
  if (rxBuf_.length() == 0) return -1;
  char c = rxBuf_[0];
  rxBuf_.remove(0, 1);
  return (uint8_t)c;
}

String BleElmClient::readStringUntil(char terminator) {
  unsigned long start = millis();
  String out;
  while (millis() - start < ELM_TIMEOUT_MS) {
    while (available()) {
      char c = (char)read();
      if (c == terminator) return out;
      if (c != '\0') out += c;
    }
    delay(5);
  }
  return out;
}

void BleElmClient::flushInput() {
  rxBuf_ = "";
}
