#pragma once

#include <Arduino.h>

namespace OtaPortal {
bool begin();          // start Wi-Fi AP + web updater
void loop();           // call frequently while active
void stop();           // stop AP / server
bool isActive();
const char* ssid();
const char* password();
String apIp();         // e.g. 192.168.4.1
}
