#include <Wire.h>

// Common ESP32-C6 I2C pin pairs (SDA, SCL)
const int pairs[][2] = {
  {6, 7},
  {19, 18},
  {8, 9},
  {5, 4},
  {21, 22},
  {4, 5},
  {7, 6},
  {9, 8},
  {18, 19},
  {10, 11},
  {1, 0},
  {2, 3},
  {20, 21},
  {15, 14},
  {23, 22},
};

void scanPair(int sda, int scl) {
  Wire.end();
  delay(20);
  Wire.begin(sda, scl, 100000);

  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  FOUND 0x%02X on SDA=%d SCL=%d\n", addr, sda, scl);
      found++;
    }
  }
  if (!found) {
    Serial.printf("  none on SDA=%d SCL=%d\n", sda, scl);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== ESP32-C6 I2C pin scanner ===");
  Serial.printf("Chip: %s\n", ESP.getChipModel());

  for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
    Serial.printf("Scan pair %u...\n", (unsigned)i);
    scanPair(pairs[i][0], pairs[i][1]);
    delay(50);
  }
  Serial.println("=== Done ===");
}

void loop() {
  delay(5000);
}
