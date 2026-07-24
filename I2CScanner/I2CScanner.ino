#include <Arduino.h>
#include <Wire.h>

void VextON() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW); // на Heltec V3 LOW = питание включено
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  VextON();
  delay(100); // дать питанию сенсоров стабилизироваться

  Wire1.begin(SDA, SCL); // SDA=41, SCL=42 (внешняя шина, см. pins_arduino.h)

  Serial.println("\nI2C сканер запущен (шина Wire1, GPIO41/42)...");
}

void loop() {
  byte error, address;
  int found = 0;

  Serial.println("Сканирование...");

  for (address = 1; address < 127; address++) {
    Wire1.beginTransmission(address);
    error = Wire1.endTransmission();

    if (error == 0) {
      Serial.print("Устройство найдено на адресе 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
      found++;
    } else if (error == 4) {
      Serial.print("Ошибка на адресе 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
    }
  }

  if (found == 0) {
    Serial.println("Устройства не найдены. Проверьте питание (Vext) и разводку SDA/SCL.");
  } else {
    Serial.print("Найдено устройств: ");
    Serial.println(found);
  }

  Serial.println("----------------------------");
  delay(3000);
}