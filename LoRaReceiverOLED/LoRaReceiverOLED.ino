/* Heltec Automation Receive + OLED output
 */

#include "LoRaWan_APP.h"
#include "Arduino.h"

// OLED (I2C, встроенный дисплей Heltec)
#include <Wire.h>
#include "HT_SSD1306Wire.h"

static SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

/*#define RF_FREQUENCY                                915000000 // Hz */
#define RF_FREQUENCY                                868900000 // Hz


#define TX_OUTPUT_POWER                             14        // dBm

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

char txpacket[BUFFER_SIZE];
char rxpacket[BUFFER_SIZE];

static RadioEvents_t RadioEvents;

int16_t txNumber;

int16_t rssi, rxSize;
int8_t  lastSnr = 0;
uint32_t rxCount = 0;

bool lora_idle = true;

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

// Экран ожидания приёма
void drawWaitingScreen()
{
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0,  "LoRa Receiver");
  display.drawString(0, 12, "Waiting for packet...");
  display.drawString(0, 24, "RX total: " + String(rxCount));
  display.display();
}

// Экран с данными последнего принятого пакета (компактная раскладка, шрифт 10pt)
void drawPacketScreen()
{
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  // Строка 0: номер пакета
  display.drawString(0, 0, "LoRa RX #" + String(rxCount));

  // Строка 1: сам пакет (ограничение по ширине на случай длинного текста)
  display.drawStringMaxWidth(0, 12, 128, String(rxpacket));

  // Строка 2: RSSI и SNR в одной строке
  display.drawString(0, 32, "RSSI:" + String(rssi) + " SNR:" + String(lastSnr));

  // Строка 3: длина пакета
  display.drawString(0, 44, "Len: " + String(rxSize) + " B");

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

    txNumber = 0;
    rssi = 0;

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
  if(lora_idle)
  {
    lora_idle = false;
    Serial.println("into RX mode");
    Radio.Rx(0);
  }
  Radio.IrqProcess( );
}

void OnRxDone( uint8_t *payload, uint16_t size, int16_t rssi_in, int8_t snr )
{
    rssi = rssi_in;
    lastSnr = snr;
    rxSize = size;
    rxCount++;

    memcpy(rxpacket, payload, size);
    rxpacket[size] = '\0';

    Radio.Sleep( );

    Serial.printf("\r\nreceived packet => \"%s\" with rssi %d , length %d\r\n", rxpacket, rssi, rxSize);

    drawPacketScreen();

    lora_idle = true;
}
