/* Heltec Automation — приёмник метеоданных по LoRa + OLED
 *
 * Принимает бинарную структуру WeatherPacket (см. передатчик):
 *   - packetId     — счётчик пакетов
 *   - temperature   — температура, °C
 *   - pressure      — давление, гПа
 *   - humidity      — влажность, %RH
 *   - illumination  — освещённость, лк
 * */

#include "LoRaWan_APP.h"
#include "Arduino.h"

// OLED (I2C, встроенный дисплей Heltec)
#include <Wire.h>
#include "HT_SSD1306Wire.h"

static SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

/*#define RF_FREQUENCY                                915000000 // Hz */
#define RF_FREQUENCY                                868900000 // Hz

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

// ---------- Структура принимаемых метеоданных ----------
// ВАЖНО: должна побайтово совпадать со структурой на передатчике
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

WeatherPacket rxPacket;

static RadioEvents_t RadioEvents;

int16_t rssi;
int8_t  lastSnr = 0;
int16_t rxSize = 0;
uint32_t rxCount = 0;       // сколько пакетов реально принято
uint32_t lastPacketId = 0;  // последний packetId для контроля пропусков
uint32_t lostCount = 0;     // оценка потерянных пакетов (по разрывам packetId)
bool havePacket = false;    // был ли принят хотя бы один валидный пакет

bool lora_idle = true;

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

// Экран ожидания приёма
void drawWaitingScreen()
{
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0,  "Weather Receiver");
  display.drawString(0, 12, "Waiting for packet...");
  display.drawString(0, 24, "RX total: " + String(rxCount));
  display.drawString(0, 36, "Lost (est): " + String(lostCount));
  display.display();
}

// Экран с данными последнего принятого пакета
void drawPacketScreen()
{
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  // Строка 0: номер пакета + служебная информация о приёме
  display.drawString(0, 0, "RX #" + String(rxPacket.packetId) +
                            "  RSSI:" + String(rssi) + " SNR:" + String(lastSnr));

  // Метеоданные
  display.drawString(0, 14, "T: "   + String(rxPacket.temperature, 1)  + " C");
  display.drawString(0, 26, "P: "   + String(rxPacket.pressure, 1)     + " hPa");
  display.drawString(0, 38, "H: "   + String(rxPacket.humidity, 1)     + " %");
  // display.drawString(0, 50, "Lux: " + String(rxPacket.illumination, 0) + " lx (" + String(rxSize) + "B)");

  display.display();
}

void setup() {
    Serial.begin(115200);
    Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);

    // Инициализация OLED
    VextON();
    delay(100);
    display.init();
    display.setFont(ArialMT_Plain_10);

    RadioEvents.RxDone = OnRxDone;
    Radio.Init( &RadioEvents );
    Radio.SetChannel( RF_FREQUENCY );
    Radio.SetRxConfig( MODEM_LORA, LORA_BANDWIDTH, LORA_SPREADING_FACTOR,
                               LORA_CODINGRATE, 0, LORA_PREAMBLE_LENGTH,
                               LORA_SYMBOL_TIMEOUT, LORA_FIX_LENGTH_PAYLOAD_ON,
                               0, true, 0, 0, LORA_IQ_INVERSION_ON, true );

    drawWaitingScreen();
}

void loop()
{
  if (lora_idle)
  {
    lora_idle = false;
    Serial.println("into RX mode");
    Radio.Rx(0);
  }
  Radio.IrqProcess();
}

void OnRxDone( uint8_t *payload, uint16_t size, int16_t rssi_in, int8_t snr )
{
    rssi    = rssi_in;
    lastSnr = snr;
    rxSize  = size;

    Radio.Sleep();

    // Принимаем пакет, только если размер совпадает с ожидаемой структурой —
    // иначе это "мусор" или пакет другого формата
    if (size == BUFFER_SIZE)
    {
        memcpy(&rxPacket, payload, BUFFER_SIZE);

        // Оценка потерь по разрывам в packetId (не считаем самый первый пакет)
        if (havePacket && rxPacket.packetId > lastPacketId + 1)
        {
            lostCount += (rxPacket.packetId - lastPacketId - 1);
        }
        lastPacketId = rxPacket.packetId;
        havePacket = true;

        rxCount++;

        Serial.printf("\r\nreceived packet #%lu: T=%.1fC P=%.1fhPa H=%.1f%% Lux=%.0f  RSSI=%d SNR=%d len=%d\r\n",
                      (unsigned long)rxPacket.packetId, rxPacket.temperature,
                      rxPacket.pressure, rxPacket.humidity, rxPacket.illumination,
                      rssi, snr, size);

        drawPacketScreen();
    }
    else
    {
        Serial.printf("\r\nreceived packet with unexpected size %d (expected %d), ignoring\r\n",
                      size, (int)BUFFER_SIZE);
        drawWaitingScreen();
    }

    lora_idle = true;
}