#include <Arduino.h>
#include <Wire.h>
#include "HT_SSD1306Wire.h"
#include <Adafruit_BME280.h>

// ---------- OLED: внутренняя шина, GPIO17/18 (не трогаем, идёт через Wire) ----------
static SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

// ---------- Внешний датчик: отдельная шина Wire1, GPIO41(SDA)/42(SCL) ----------
Adafruit_BME280 bme;

bool bme_ok = false;
uint8_t bmeAddr = 0x76;
uint8_t bmeChipId = 0x00;

// Читает регистр 0xD0 (Chip ID) по заданному адресу
uint8_t readChipId(uint8_t addr) {
  Wire1.beginTransmission(addr);
  Wire1.write(0xD0);
  if (Wire1.endTransmission(false) != 0) return 0x00; // устройство не ответило
  Wire1.requestFrom((int)addr, 1);
  if (Wire1.available()) {
    return Wire1.read();
  }
  return 0x00;
}

void VextON() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW); // на Heltec V3 LOW = питание включено
}

void setup() {
  Serial.begin(115200);
  delay(200);

  VextON();
  delay(300); // дать питанию стабилизироваться

  // Инициализация OLED
  display.init();
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 0, "Init sensors...");
  display.display();

  // Инициализация внешней I2C-шины (GPIO41/42) для датчика
  Wire1.begin(SDA, SCL); // SDA=41, SCL=42 — определены в pins_arduino.h
  Wire1.setClock(100000);

  // Пробуем сначала 0x76, если не отвечает — 0x77
  bmeChipId = readChipId(0x76);
  if (bmeChipId == 0x00) {
    bmeAddr = 0x77;
    bmeChipId = readChipId(0x77);
  } else {
    bmeAddr = 0x76;
  }

  Serial.printf("Sensor addr=0x%02X, Chip ID=0x%02X\n", bmeAddr, bmeChipId);

  display.clear();
  display.drawString(0, 0, "Chip ID: 0x" + String(bmeChipId, HEX));
  display.drawString(0, 12, "Addr: 0x" + String(bmeAddr, HEX));
  display.display();
  delay(1500);

  if (bmeChipId == 0x00) {
    Serial.println("Датчик не отвечает на шине (ни 0x76, ни 0x77)!");
    bme_ok = false;
  } else {
    // begin() у BME280 принимает адрес и указатель на TwoWire
    bme_ok = bme.begin(bmeAddr, &Wire1);
    if (!bme_ok) {
      Serial.println("BME280 begin() вернул false!");
    }
  }

  delay(500);
}

void loop() {
  float temp = NAN, press = NAN, hum = NAN;

  if (bme_ok) {
    temp = bme.readTemperature();
    press = bme.readPressure() / 100.0F; // гПа
    hum = bme.readHumidity();            // %
  }

  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);

  if (bme_ok) {
    display.drawString(0, 0, "BME280:");
    display.drawString(0, 12, "T: " + String(temp, 1) + " C");
    display.drawString(0, 24, "P: " + String(press, 0) + " hPa");
    display.drawString(0, 36, "RH: " + String(hum, 1) + " %");
  } else {
    display.drawString(0, 0, "BME280: ошибка");
    display.drawString(0, 12, "ID: 0x" + String(bmeChipId, HEX));
  }

  display.display();

  if (bme_ok) {
    Serial.printf("BME280 T=%.2f C  P=%.2f hPa  RH=%.1f %%\n", temp, press, hum);
  }

  delay(2000);
}