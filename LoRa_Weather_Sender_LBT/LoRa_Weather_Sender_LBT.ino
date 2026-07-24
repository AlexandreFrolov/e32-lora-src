/* Heltec Automation — передатчик метеоданных BME280 по LoRa + OLED
 * С поддержкой LBT (Listen Before Talk) и циклом Deep Sleep (1 минута)
 */

#include "LoRaWan_APP.h"
#include "Arduino.h"

// OLED (I2C, встроенный дисплей Heltec)
#include <Wire.h>
#include "HT_SSD1306Wire.h"

// BME280
#include <Adafruit_BME280.h>

static SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

#define RF_FREQUENCY                 868900000 // Hz
#define TX_OUTPUT_POWER              -9        // dBm

#define LORA_BANDWIDTH               0         // 125 kHz
#define LORA_SPREADING_FACTOR        7         // SF7
#define LORA_CODINGRATE              1         // 4/5
#define LORA_PREAMBLE_LENGTH         8         
#define LORA_SYMBOL_TIMEOUT          0         
#define LORA_FIX_LENGTH_PAYLOAD_ON   false
#define LORA_IQ_INVERSION_ON         false

#define RX_TIMEOUT_VALUE             1000

// ---------- Настройки Deep Sleep ----------
#define US_TO_S_FACTOR               1000000ULL /* Множитель для перевода микросекунд в секунды */
#define TIME_TO_SLEEP                60         /* Время сна в секундах (1 минута) */

// ---------- Параметры LBT ----------
#define LBT_RSSI_THRESHOLD           -80       // dBm
#define LBT_CARRIER_SENSE_TIME       5         // ms
#define LBT_MAX_ATTEMPTS             10        
#define LBT_BACKOFF_MIN_MS           10        
#define LBT_BACKOFF_MAX_MS           50        

// ---------- Структура данных ----------
#pragma pack(push, 1)
typedef struct {
  uint32_t packetId;      // номер пакета
  float    temperature;   // °C
  float    pressure;      // гПа
  float    humidity;      // %RH
  float    illumination;  // лк
} WeatherPacket;
#pragma pack(pop)

#define BUFFER_SIZE                  sizeof(WeatherPacket)

WeatherPacket txPacket;
uint8_t txBuffer[BUFFER_SIZE];

// Переменные в RTC-памяти (сохраняются при Deep Sleep)
RTC_DATA_ATTR uint32_t txCount = 0;
RTC_DATA_ATTR uint32_t txSkippedCount = 0;

bool lora_idle = true;
bool lastTxOk = true;

// ---------- BME280 ----------
Adafruit_BME280 bme;
bool bme_ok = false;
uint8_t bmeAddr = 0x76;
uint8_t bmeChipId = 0x00;

static RadioEvents_t RadioEvents;
void OnTxDone( void );
void OnTxTimeout( void );

void VextON(void) {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
}

void VextOFF(void) {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, HIGH);
}

// Чтение Chip ID по заданному I2C адресу (Wire1)
uint8_t readChipId(uint8_t addr) {
  Wire1.beginTransmission(addr);
  Wire1.write(0xD0);
  if (Wire1.endTransmission(false) != 0) return 0x00;
  Wire1.requestFrom((int)addr, 1);
  if (Wire1.available()) {
    return Wire1.read();
  }
  return 0x00;
}

void initBME280() {
  Wire1.begin(SDA, SCL); // GPIO41/GPIO42 по умолчанию
  Wire1.setClock(100000);

  // Опрос 0x76 -> 0x77
  bmeChipId = readChipId(0x76);
  if (bmeChipId == 0x00) {
    bmeAddr = 0x77;
    bmeChipId = readChipId(0x77);
  } else {
    bmeAddr = 0x76;
  }

  Serial.printf("BME280 check: Addr=0x%02X, ChipID=0x%02X\n", bmeAddr, bmeChipId);

  if (bmeChipId != 0x00) {
    bme_ok = bme.begin(bmeAddr, &Wire1);
    if (!bme_ok) {
      Serial.println("BME280 begin() failed!");
    } else {
      Serial.println("BME280 init success!");
    }
  } else {
    Serial.println("BME280 not found!");
    bme_ok = false;
  }
}

void fillWeatherPacket(WeatherPacket &p) {
  p.packetId = txCount + 1;

  if (bme_ok) {
    p.temperature  = bme.readTemperature();
    p.pressure     = bme.readPressure() / 100.0F;
    p.humidity     = bme.readHumidity();
  } else {
    // В случае ошибки сенсора передаем NAN
    p.temperature  = NAN;
    p.pressure     = NAN;
    p.humidity     = NAN;
  }
  
  p.illumination = 0.0F; // Резерв под люксметр
}

// ---------- Отрисовка OLED ----------
void drawSendingScreen(const WeatherPacket &p) {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  display.drawString(0, 0, "TX #" + String(p.packetId) + " Sending...");

  if (bme_ok) {
    display.drawString(0, 14, "T: " + String(p.temperature, 1) + " C");
    display.drawString(0, 26, "P: " + String(p.pressure, 1)    + " hPa");
    display.drawString(0, 38, "H: " + String(p.humidity, 1)    + " %");
  } else {
    display.drawString(0, 20, "BME280 ERROR!");
  }

  display.display();
}

void drawStatusScreen(const WeatherPacket &p) {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  display.drawString(0, 0, "TX #" + String(p.packetId) + " " + (lastTxOk ? "OK" : "TIMEOUT"));

  if (bme_ok) {
    display.drawString(0, 14, "T: " + String(p.temperature, 1) + " C");
    display.drawString(0, 26, "P: " + String(p.pressure, 1)    + " hPa");
    display.drawString(0, 38, "H: " + String(p.humidity, 1)    + " %");
  } else {
    display.drawString(0, 20, "BME280 ERROR!");
  }

  display.display();
}

void drawChannelBusyScreen(uint8_t attempt) {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "TX #" + String(txCount + 1));
  display.setFont(ArialMT_Plain_16);
  display.drawString(0, 20, "Channel busy");
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 40, "LBT retry " + String(attempt) + "/" + String(LBT_MAX_ATTEMPTS));
  display.drawString(0, 54, "Skipped: " + String(txSkippedCount));
  display.display();
}

bool waitForFreeChannel() {
  bool channelFree = false;
  uint8_t attempt = 0;

  while (!channelFree && attempt < LBT_MAX_ATTEMPTS) {
    channelFree = Radio.IsChannelFree(MODEM_LORA, RF_FREQUENCY,
                                       LBT_RSSI_THRESHOLD,
                                       LBT_CARRIER_SENSE_TIME);
    if (!channelFree) {
      attempt++;
      drawChannelBusyScreen(attempt);
      delay(random(LBT_BACKOFF_MIN_MS, LBT_BACKOFF_MAX_MS));
    }
  }
  return channelFree;
}

void goToSleep() {
  Serial.println("Going to Deep Sleep for 60 seconds...");
  
  // Управление периферией перед сном
  Radio.Sleep();      // Перевод радиомодуля SX1262 в спящий режим
  display.sleep();    // Перевод дисплея в спящий режим
  VextOFF();          // Отключение питания внешних датчиков (BME280) и OLED
  
  // Настройка таймера пробуждения
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * US_TO_S_FACTOR);
  
  // Переход в глубокий сон
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);

  VextON();
  delay(100);

  display.init();
  display.setFont(ArialMT_Plain_10);

  // Инициализация датчика BME280 на Wire1
  initBME280();

  randomSeed(esp_random());

  RadioEvents.TxDone = OnTxDone;
  RadioEvents.TxTimeout = OnTxTimeout;

  Radio.Init(&RadioEvents);
  Radio.SetChannel(RF_FREQUENCY);
  Radio.SetTxConfig(MODEM_LORA, TX_OUTPUT_POWER, 0, LORA_BANDWIDTH,
                    LORA_SPREADING_FACTOR, LORA_CODINGRATE,
                    LORA_PREAMBLE_LENGTH, LORA_FIX_LENGTH_PAYLOAD_ON,
                    true, 0, 0, LORA_IQ_INVERSION_ON, 3000);

  // --- Выполнение передачи ---
  fillWeatherPacket(txPacket);
  memcpy(txBuffer, &txPacket, BUFFER_SIZE);

  if (!waitForFreeChannel()) {
    txSkippedCount++;
    Serial.println("LBT: Channel busy, skipping TX");
    goToSleep();
  }

  Serial.printf("\r\nTX #%lu: T=%.1f C, P=%.1f hPa, H=%.1f %%\r\n",
                (unsigned long)txPacket.packetId, txPacket.temperature,
                txPacket.pressure, txPacket.humidity);

  txCount++;
  drawSendingScreen(txPacket);

  lora_idle = false;
  Radio.Send(txBuffer, BUFFER_SIZE);

  // Ожидание завершения передачи (обработка прерываний SX1262)
  while (!lora_idle) {
    Radio.IrqProcess();
  }

  // Задержка на 2 секунды, чтобы успеть рассмотреть результат на экране
  delay(2000);

  // Переход в режим глубокого сна
  goToSleep();
}

void loop() {
  // Вся логика выполняется в setup(). Пустой loop() никогда не выполнится,
  // так как в конце setup() плата уходит в deep sleep и перезагружается.
}

void OnTxDone(void) {
  Serial.println("TX Done");
  lastTxOk = true;
  drawStatusScreen(txPacket);
  lora_idle = true;
}

void OnTxTimeout(void) {
  Radio.Sleep();
  Serial.println("TX Timeout");
  lastTxOk = false;
  drawStatusScreen(txPacket);
  lora_idle = true;
}