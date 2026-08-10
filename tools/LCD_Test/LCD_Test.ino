#include <Arduino_GFX_Library.h>

// Waveshare ESP32-C6-LCD-1.47
#define LCD_DC   15
#define LCD_CS   14
#define LCD_SCK  7
#define LCD_MOSI 6
#define LCD_MISO 5
#define LCD_RST  21
#define LCD_BL   22
#define SD_CS    4

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO);
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, LCD_RST, 1 /* landscape */, true /* IPS */,
    172, 320, 34, 0, 34, 0);

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("LCD test Waveshare C6-1.47");

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  if (!gfx->begin()) {
    Serial.println("gfx->begin() FAILED");
  }

  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextSize(3);
  gfx->setCursor(20, 40);
  gfx->setTextColor(RGB565_GREEN);
  gfx->println("LCD OK");
  gfx->setTextSize(2);
  gfx->setCursor(20, 90);
  gfx->setTextColor(RGB565_WHITE);
  gfx->println("1.47 ST7789");
  gfx->fillRect(20, 130, 280, 20, RGB565_RED);
  Serial.println("Drawn test pattern");
}

void loop() { delay(1000); }
