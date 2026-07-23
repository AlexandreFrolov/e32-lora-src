/* Heltec Automation — передатчик метеоданных по LoRa + OLED
 * с поддержкой LBT (Listen Before Talk)
 *
 * Передаётся бинарная структура WeatherPacket:
 *   - packetId     — счётчик пакетов
 *   - temperature   — температура, °C
 *   - pressure      — давление, гПа
 *   - humidity      — влажность, %RH
 *   - illumination  — освещённость, лк
 *
 * Пока данные генерируются программно (плавный "случайный блуждающий"
 * сигнал), позже эту функцию можно заменить чтением реальных сенсоров
 * (BMP-380/388 для давления/температуры, датчик влажности, фоторезистор
 * или люксметр для освещённости).
 * */

#include "LoRaWan_APP.h"
#include "Arduino.h"

// OLED (I2C, встроенный дисплей Heltec)
#include <Wire.h>
#include "HT_SSD1306Wire.h"

static SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

/*#define RF_FREQUENCY                                915000000 // Hz */
#define RF_FREQUENCY                                868900000 // Hz

#define TX_OUTPUT_POWER                             -9        // dBm

#define LORA_BANDWIDTH                              0         // [0: 125 kHz,
                                                              //  1: 250 kHz,
                                                              //  2: 500 kHz,
                                                              //  3: Reserved]
#define LORA_SPREADING_FACTOR                       7         // [SF7..SF12]
#define LORA_CODINGRATE                             1         // [1: 4/5,
                                                              //  2: 4/6,
                                                              //  3: 4/7,
                                                              //  4: 4/8]
#define LORA_PREAMBLE_LENGTH                        8         // Same for Tx and Rx
#define LORA_SYMBOL_TIMEOUT                         0         // Symbols
#define LORA_FIX_LENGTH_PAYLOAD_ON                  false
#define LORA_IQ_INVERSION_ON                        false

#define RX_TIMEOUT_VALUE                            1000

// ---------- Параметры LBT (Listen Before Talk) ----------
#define LBT_RSSI_THRESHOLD                          -80  // dBm, порог "канал занят"
#define LBT_CARRIER_SENSE_TIME                       5    // ms, время одного замера RSSI
#define LBT_MAX_ATTEMPTS                             10   // максимум попыток прослушки за цикл
#define LBT_BACKOFF_MIN_MS                           10   // мин. пауза перед повторной попыткой
#define LBT_BACKOFF_MAX_MS                           50   // макс. пауза перед повторной попыткой
// ----------------------------------------------------------

// ---------- Структура передаваемых метеоданных ----------
#pragma pack(push, 1)
typedef struct {
  uint32_t packetId;      // порядковый номер пакета
  float    temperature;   // °C
  float    pressure;      // гПа (hPa)
  float    humidity;      // %RH
  float    illumination;  // лк (lux)
} WeatherPacket;
#pragma pack(pop)

#define BUFFER_SIZE                                 sizeof(WeatherPacket)

WeatherPacket txPacket;
uint8_t txBuffer[BUFFER_SIZE];

bool lora_idle = true;

uint32_t txCount = 0;
uint32_t txSkippedCount = 0; // сколько раз пропустили передачу из-за занятого канала
bool lastTxOk = true;        // true = TX done, false = TX timeout

static RadioEvents_t RadioEvents;
void OnTxDone( void );
void OnTxTimeout( void );

// Включение/выключение питания OLED (Vext) на платах Heltec
void VextON(void)
{
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
}

void VextOFF(void)
{
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, HIGH);
}

// ---------- Генерация тестовых метеоданных ----------
// Плавное "блуждание" вокруг реалистичных значений — имитация показаний
// погодной станции до подключения реальных сенсоров.
float generateTemperature()
{
  // база 20°C + суточный синус ±5°C + шум ±1°C
  float base  = 20.0 + 5.0 * sin(millis() / 60000.0);
  float noise = random(-100, 100) / 100.0;
  return base + noise;
}

float generatePressure()
{
  // база 1013 гПа + медленный дрейф ±10 гПа + шум ±0.5 гПа
  float base  = 1013.0 + 10.0 * sin(millis() / 180000.0 + 1.0);
  float noise = random(-50, 50) / 100.0;
  return base + noise;
}

float generateHumidity()
{
  // база 55% + колебания ±20% + шум ±1%, ограничение диапазона 0..100
  float base  = 55.0 + 20.0 * sin(millis() / 120000.0 + 2.0);
  float noise = random(-100, 100) / 100.0;
  float value = base + noise;
  if (value < 0)   value = 0;
  if (value > 100) value = 100;
  return value;
}

float generateIllumination()
{
  // база 500 лк (пасмурный день) + "облачность" шум ±300 лк,
  // ограничение снизу нулём (ночь/темнота)
  float base  = 500.0 + 400.0 * sin(millis() / 90000.0 + 0.5);
  float noise = random(-300, 300);
  float value = base + noise;
  if (value < 0) value = 0;
  return value;
}

void fillWeatherPacket(WeatherPacket &p)
{
  p.packetId    = txCount + 1;
  p.temperature = generateTemperature();
  p.pressure    = generatePressure();
  p.humidity    = generateHumidity();
  p.illumination= generateIllumination();
}

// ---------- Экраны OLED ----------

// Экран с отправляемым пакетом (пока идёт передача)
void drawSendingScreen(const WeatherPacket &p)
{
  display.clear();

  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "TX #" + String(p.packetId) + "  Sending...");

  display.drawString(0, 14, "T: "   + String(p.temperature, 1)  + " C");
  display.drawString(0, 26, "P: "   + String(p.pressure, 1)     + " hPa");
  display.drawString(0, 38, "H: "   + String(p.humidity, 1)     + " %");
  display.drawString(0, 50, "Lux: " + String(p.illumination, 0) + " lx");

  display.display();
}

// Экран с итогом передачи (успех / таймаут)
void drawStatusScreen(const WeatherPacket &p)
{
  display.clear();

  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "TX #" + String(p.packetId) + "  " + (lastTxOk ? "OK" : "TIMEOUT"));

  display.drawString(0, 14, "T: "   + String(p.temperature, 1)  + " C");
  display.drawString(0, 26, "P: "   + String(p.pressure, 1)     + " hPa");
  display.drawString(0, 38, "H: "   + String(p.humidity, 1)     + " %");
  display.drawString(0, 50, "Lux: " + String(p.illumination, 0) + " lx");

  display.display();
}

// Экран "канал занят, идёт прослушка / бэкофф"
void drawChannelBusyScreen(uint8_t attempt)
{
  display.clear();

  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "TX #" + String(txCount + 1));

  display.setFont(ArialMT_Plain_16);
  display.drawString(0, 20, "Channel busy");

  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 40, "LBT retry " + String(attempt) + "/" + String(LBT_MAX_ATTEMPTS));
  display.drawString(0, 54, "Skipped total: " + String(txSkippedCount));

  display.display();
}

// Возвращает true, если канал свободен (пройден LBT), false — если не удалось
// дождаться свободного канала за LBT_MAX_ATTEMPTS попыток
bool waitForFreeChannel()
{
  bool channelFree = false;
  uint8_t attempt = 0;

  while (!channelFree && attempt < LBT_MAX_ATTEMPTS)
  {
    channelFree = Radio.IsChannelFree(MODEM_LORA, RF_FREQUENCY,
                                       LBT_RSSI_THRESHOLD,
                                       LBT_CARRIER_SENSE_TIME);

    if (!channelFree)
    {
      attempt++;
      Serial.printf("LBT: channel busy, attempt %d/%d\r\n", attempt, LBT_MAX_ATTEMPTS);
      drawChannelBusyScreen(attempt);
      delay(random(LBT_BACKOFF_MIN_MS, LBT_BACKOFF_MAX_MS));
    }
  }

  return channelFree;
}

void setup() {
    Serial.begin(115200);
    Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);

    // Инициализация OLED
    VextON();
    delay(100);
    display.init();
    display.setFont(ArialMT_Plain_10);

    randomSeed(esp_random());

    RadioEvents.TxDone = OnTxDone;
    RadioEvents.TxTimeout = OnTxTimeout;

    Radio.Init( &RadioEvents );
    Radio.SetChannel( RF_FREQUENCY );
    Radio.SetTxConfig( MODEM_LORA, TX_OUTPUT_POWER, 0, LORA_BANDWIDTH,
                                   LORA_SPREADING_FACTOR, LORA_CODINGRATE,
                                   LORA_PREAMBLE_LENGTH, LORA_FIX_LENGTH_PAYLOAD_ON,
                                   true, 0, 0, LORA_IQ_INVERSION_ON, 3000 );

    display.clear();
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_10);
    display.drawString(0, 0, "Weather Sender (LBT)");
    display.drawString(0, 20, "Payload: " + String(BUFFER_SIZE) + " bytes");
    display.drawString(0, 32, "Ready to send...");
    display.display();
}

void loop()
{
	if (lora_idle == true)
	{
		delay(1000);

		// Формируем пакет с метеоданными
		fillWeatherPacket(txPacket);
		memcpy(txBuffer, &txPacket, BUFFER_SIZE);

		// --- LBT: слушаем эфир перед отправкой ---
		if (!waitForFreeChannel())
		{
			// Канал так и не освободился за отведённое число попыток —
			// пропускаем этот цикл передачи
			txSkippedCount++;
			Serial.println("LBT: channel still busy, skipping this TX cycle");
			lora_idle = true;
			return;
		}

		Serial.printf("\r\nsending packet #%lu: T=%.1fC P=%.1fhPa H=%.1f%% Lux=%.0f (len=%d)\r\n",
		              (unsigned long)txPacket.packetId, txPacket.temperature,
		              txPacket.pressure, txPacket.humidity, txPacket.illumination,
		              (int)BUFFER_SIZE);

		txCount++;
		drawSendingScreen(txPacket);

		Radio.Send( txBuffer, BUFFER_SIZE ); // отправляем бинарный пакет
		lora_idle = false;
	}
	Radio.IrqProcess();
}

void OnTxDone( void )
{
	Serial.println("TX done......");
	lastTxOk = true;
	drawStatusScreen(txPacket);
	lora_idle = true;
}

void OnTxTimeout( void )
{
    Radio.Sleep();
    Serial.println("TX Timeout......");
    lastTxOk = false;
    drawStatusScreen(txPacket);
    lora_idle = true;
}
