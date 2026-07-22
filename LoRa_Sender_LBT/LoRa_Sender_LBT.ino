/* Heltec Automation send communication test example + OLED output
 * с поддержкой LBT (Listen Before Talk)
 *
 * Function:
 * 1. Send data from a esp32 device over hardware
 * 2. Перед отправкой прослушивать канал (RSSI-based LBT) и,
 *    если канал занят, ждать с рандомизированным бэкоффом
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
#define BUFFER_SIZE                                 30 // Define the payload size here

// ---------- Параметры LBT (Listen Before Talk) ----------
#define LBT_RSSI_THRESHOLD                          -80  // dBm, порог "канал занят"
#define LBT_CARRIER_SENSE_TIME                       5    // ms, время одного замера RSSI
#define LBT_MAX_ATTEMPTS                             10   // максимум попыток прослушки за цикл
#define LBT_BACKOFF_MIN_MS                           10   // мин. пауза перед повторной попыткой
#define LBT_BACKOFF_MAX_MS                           50   // макс. пауза перед повторной попыткой
// ----------------------------------------------------------

char txpacket[BUFFER_SIZE];
char rxpacket[BUFFER_SIZE];

double txNumber;

bool lora_idle=true;

uint32_t txCount = 0;
uint32_t txSkippedCount = 0; // сколько раз пропустили передачу из-за занятого канала
bool lastTxOk = true; // true = TX done, false = TX timeout

static RadioEvents_t RadioEvents;
void OnTxDone( void );
void OnTxTimeout( void );

// Включение питания OLED (Vext) на платах Heltec
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

// Экран с только что отправленным пакетом (пока идёт передача)
void drawSendingScreen()
{
  display.clear();

  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "LoRa TX #" + String(txCount));

  display.setFont(ArialMT_Plain_16);
  display.drawStringMaxWidth(0, 14, 128, String(txpacket));

  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 54, "Sending...");

  display.display();
}

// Экран с итогом передачи (успех / таймаут)
void drawStatusScreen()
{
  display.clear();

  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "LoRa TX #" + String(txCount));

  display.setFont(ArialMT_Plain_16);
  display.drawStringMaxWidth(0, 14, 128, String(txpacket));

  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 54, lastTxOk ? "TX done" : "TX timeout");

  display.display();
}

// Экран "канал занят, идёт прослушка / бэкофф"
void drawChannelBusyScreen(uint8_t attempt)
{
  display.clear();

  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "LoRa TX #" + String(txCount + 1));

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
    Mcu.begin(HELTEC_BOARD,SLOW_CLK_TPYE);

    // Инициализация OLED
    VextON();
    delay(100);
    display.init();
    display.setFont(ArialMT_Plain_10);

    randomSeed(esp_random());

    txNumber=0;

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
    display.drawString(0, 0, "LoRa Sender (LBT)");
    display.drawString(0, 20, "Ready to send...");
    display.display();
   }

void loop()
{
	if(lora_idle == true)
	{
    delay(1000);
		txNumber += 0.01;
		sprintf(txpacket,"Hello LoRa %0.2f",txNumber);  //start a package

		// --- LBT: слушаем эфир перед отправкой ---
		if (!waitForFreeChannel())
		{
			// Канал так и не освободился за отведённое число попыток —
			// пропускаем этот цикл передачи, не превышая max attempts
			txSkippedCount++;
			Serial.println("LBT: channel still busy, skipping this TX cycle");
			lora_idle = true;
			return;
		}

		Serial.printf("\r\nsending packet \"%s\" , length %d\r\n",txpacket, strlen(txpacket));

		txCount++;
		drawSendingScreen();

		Radio.Send( (uint8_t *)txpacket, strlen(txpacket) ); //send the package out	
    lora_idle = false;
	}
  Radio.IrqProcess( );
}

void OnTxDone( void )
{
	Serial.println("TX done......");
	lastTxOk = true;
	drawStatusScreen();
	lora_idle = true;
}

void OnTxTimeout( void )
{
    Radio.Sleep( );
    Serial.println("TX Timeout......");
    lastTxOk = false;
    drawStatusScreen();
    lora_idle = true;
}
