#include <Arduino.h>
#include <Wire.h>

void VextON() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
}

// Кандидаты {SDA, SCL} — безопасные для внешнего использования пины Heltec V3,
// плюс официальная пара 41/42, плюс на всякий случай зеркальные варианты
int candidates[][2] = {
  {41, 42}, {42, 41},   // официальная внешняя I2C-шина
  {2, 1},   {1, 2},
  {4, 5},   {5, 4},
  {6, 7},   {7, 6},
  {19, 20}, {20, 19},
  {47, 48}, {48, 47},
  {17, 18}, {18, 17},   // на случай, если провода воткнуты в пады OLED-шины
};
const int numCandidates = sizeof(candidates) / sizeof(candidates[0]);

void setup() {
  Serial.begin(115200);
  delay(1000);

  VextON();
  delay(200);

  Serial.println("Автоперебор пар SDA/SCL...\n");

  bool anyFound = false;

  for (int i = 0; i < numCandidates; i++) {
    int sda = candidates[i][0];
    int scl = candidates[i][1];

    Wire1.end(); // сбросить предыдущую конфигурацию шины
    delay(20);
    Wire1.begin(sda, scl);
    Wire1.setClock(100000);
    delay(20);

    Serial.printf("Пробуем SDA=%d, SCL=%d ... ", sda, scl);

    int found = 0;
    for (byte addr = 1; addr < 127; addr++) {
      Wire1.beginTransmission(addr);
      if (Wire1.endTransmission() == 0) {
        if (found == 0) Serial.println();
        Serial.printf("  -> устройство на 0x%02X\n", addr);
        found++;
      }
    }

    if (found > 0) {
      Serial.printf("=== НАЙДЕНО: SDA=%d, SCL=%d, устройств: %d ===\n\n", sda, scl, found);
      anyFound = true;
    } else {
      Serial.println("ничего");
    }
  }

  if (!anyFound) {
    Serial.println("\nНи одна пара не сработала. Проблема физическая:");
    Serial.println("- прозвоните SDA/SCL от сенсора до конкретного отверстия на плате мультиметром (режим прозвонки)");
    Serial.println("- проверьте, не перепутаны ли VDD/GND местами на модуле");
    Serial.println("- проверьте плотность посадки проводов в макетке");
  }
}

void loop() {
  delay(10000);
}