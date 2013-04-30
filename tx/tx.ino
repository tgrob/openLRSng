/****************************************************
 * OpenLRSng transmitter code
 ****************************************************/
//extern "C" void __cxa_pure_virtual()
//{
//  cli();
//  for (;;);
//}


#include <FastSerial.h>  // this is now preventing program from running properly!
#include <BetterStream.h>
#include <AP_Common.h>
#include <AP_Math.h>

#include <Arduino.h>
#include <EEPROM.h>

#include "config.h"
#include "hardware.h"
#include "binding.h"
#include "common.h"
#include "packet.h"

#if MAVLINK_INJECT == 1
	#include "mavlink.h"
	#include "../GCS_MAVLink/include/mavlink/v1.0/mavlink_types.h"
	#include "../GCS_MAVLink/include/mavlink/v1.0/ardupilotmega/mavlink.h"
#endif

FastSerialPort0(Serial);


uint8_t RSSI_remote = 0;
uint8_t RF_channel = 0;

uint8_t FSstate = 0; // 1 = waiting timer, 2 = send FS, 3 sent waiting btn release
uint32_t FStime = 0;  // time when button went down...

uint32_t lastSent = 0;

uint32_t lastTelemetry = 0;

volatile uint8_t ppmAge = 0; // age of PPM data


volatile uint16_t startPulse = 0;
volatile uint8_t  ppmCounter = PPM_CHANNELS; // ignore data until first sync pulse

#define TIMER1_FREQUENCY_HZ 50
#define TIMER1_PRESCALER    8
#define TIMER1_PERIOD       (F_CPU/TIMER1_PRESCALER/TIMER1_FREQUENCY_HZ)

#ifdef USE_ICP1 // Use ICP1 in input capture mode
/****************************************************
 * Interrupt Vector
 ****************************************************/
ISR(TIMER1_CAPT_vect)
{
  uint16_t stopPulse = ICR1;

  // Compensate for timer overflow if needed
  uint16_t pulseWidth = ((startPulse > stopPulse) ? TIMER1_PERIOD : 0) + stopPulse - startPulse;

  if (pulseWidth > 5000) {      // Verify if this is the sync pulse (2.5ms)
    ppmCounter = 0;             // -> restart the channel counter
    ppmAge = 0;                 // brand new PPM data received
  } else if ((pulseWidth > 1400) && (ppmCounter < PPM_CHANNELS)) {       // extra channels will get ignored here
    PPM[ppmCounter] = servoUs2Bits(pulseWidth / 2);   // Store measured pulse length (converted)
    ppmCounter++;                     // Advance to next channel
  } else {
    ppmCounter = PPM_CHANNELS; // glitch ignore rest of data
  }

  startPulse = stopPulse;         // Save time at pulse start
}

void setupPPMinput()
{
  // Setup timer1 for input capture (PSC=8 -> 0.5ms precision, top at 20ms)
  TCCR1A = ((1 << WGM10) | (1 << WGM11));
  TCCR1B = ((1 << WGM12) | (1 << WGM13) | (1 << CS11) | (1 << ICES1));
  OCR1A = TIMER1_PERIOD;
  TIMSK1 |= (1 << ICIE1);   // Enable timer1 input capture interrupt
}
#else // sample PPM using pinchange interrupt
ISR(PPM_Signal_Interrupt)
{
  uint16_t time_temp;

  if (PPM_Signal_Edge_Check) {   // Only works with rising edge of the signal
    time_temp = TCNT1; // read the timer1 value
    TCNT1 = 0; // reset the timer1 value for next

    if (time_temp > 5000) {   // new frame detection (>2.5ms)
      ppmCounter = 0;             // -> restart the channel counter
      ppmAge = 0;                 // brand new PPM data received
    } else if ((time_temp > 1400) && (ppmCounter < PPM_CHANNELS)) {
      PPM[ppmCounter] = servoUs2Bits(time_temp / 2);   // Store measured pulse length (converted)
      ppmCounter++;                     // Advance to next channel
    } else {
      ppmCounter = PPM_CHANNELS; // glitch ignore rest of data
    }
  }
}

void setupPPMinput(void)
{
  // Setup timer1 for input capture (PSC=8 -> 0.5ms precision, top at 20ms)
  TCCR1A = ((1 << WGM10) | (1 << WGM11));
  TCCR1B = ((1 << WGM12) | (1 << WGM13) | (1 << CS11));
  OCR1A = TIMER1_PERIOD;
  TIMSK1 = 0;
  PPM_Pin_Interrupt_Setup
}
#endif

void handleCLI(char c)
{
  switch (c) {
  case '?':
    bindPrint();
    break;

  case '#':
    digitalWrite(BUZZER, LOW);
    scannerMode();
    break;
  }
}

void bindMode(void)
{
  uint32_t prevsend = millis();
  init_rfm(1);

  while (Serial.available()) {
    Serial.read();    // flush serial
  }

  while (1) {
    if (millis() - prevsend > 200) {
      prevsend = millis();
      Green_LED_ON;
      digitalWrite(BUZZER, HIGH);   // Buzzer on
      tx_packet((uint8_t*)&bind_data, sizeof(bind_data));
      Green_LED_OFF;
      digitalWrite(BUZZER, LOW);   // Buzzer off
    }

    while (Serial.available()) {
      handleCLI(Serial.read());
    }
  }
}

void checkButton(void)
{

  uint32_t time, loop_time;

  if (digitalRead(BTN) == 0) {     // Check the button
    delay(200);   // wait for 200mS when buzzer ON
    digitalWrite(BUZZER, LOW);   // Buzzer off

    time = millis();  //set the current time
    loop_time = time;

    while ((digitalRead(BTN) == 0) && (loop_time < time + 4800)) {
      // wait for button reelase if it is already pressed.
      loop_time = millis();
    }

    // Check the button again, If it is still down reinitialize
    if (0 == digitalRead(BTN)) {
      int8_t bzstate = HIGH;
      uint8_t doRandomize = 1;

      digitalWrite(BUZZER, bzstate);
      loop_time = millis();

      while (0 == digitalRead(BTN)) {     // wait for button to release
        if (loop_time > time + 9800) {
          digitalWrite(BUZZER, HIGH);
          doRandomize = 0;
        } else {
          if ((millis() - loop_time) > 200) {
            loop_time = millis();
            bzstate = !bzstate;
            digitalWrite(BUZZER, bzstate);
          }
        }
      }

      digitalWrite(BUZZER, LOW);
      randomSeed(micros());   // button release time in us should give us enough seed
      bindInitDefaults();
      if (doRandomize) {
        bindRandomize();
      }
      bindWriteEeprom();
      bindPrint();
    }

    // Enter binding mode, automatically after recoding or when pressed for shorter time.
    Serial.println("Entering binding mode\n");
    bindMode();
  }
}

void checkFS(void)
{

  switch (FSstate) {
  case 0:
    if (!digitalRead(BTN)) {
      FSstate = 1;
      FStime = millis();
    }

    break;

  case 1:
    if (!digitalRead(BTN)) {
      if ((millis() - FStime) > 1000) {
        FSstate = 2;
        digitalWrite(BUZZER, HIGH);   // Buzzer on
      }
    } else {
      FSstate = 0;
    }

    break;

  case 2:
    if (digitalRead(BTN)) {
      digitalWrite(BUZZER, LOW);   // Buzzer off
      FSstate = 0;
    }

    break;
  }
}

void setup(void)
{

  //RF module pins
  pinMode(SDO_pin, INPUT);   //SDO
  pinMode(SDI_pin, OUTPUT);   //SDI
  pinMode(SCLK_pin, OUTPUT);   //SCLK
  pinMode(IRQ_pin, INPUT);   //IRQ
  pinMode(nSel_pin, OUTPUT);   //nSEL

  //LED and other interfaces
  pinMode(Red_LED, OUTPUT);   //RED LED
  pinMode(Green_LED, OUTPUT);   //GREEN LED
  pinMode(BUZZER, OUTPUT);   //Buzzer
  pinMode(BTN, INPUT);   //Buton

  pinMode(PPM_IN, INPUT);   //PPM from TX
  digitalWrite(PPM_IN, HIGH); // enable pullup for TX:s with open collector output

#if defined (RF_OUT_INDICATOR)
  pinMode(RF_OUT_INDICATOR, OUTPUT);
  digitalWrite(RF_OUT_INDICATOR, LOW);
#endif

  Serial.begin(SERIAL_BAUD_RATE, SERIAL_RX_BUFFERSIZE, SERIAL_TX_BUFFERSIZE);
  Serial.set_blocking_writes(false);

  if (bindReadEeprom()) {
    Serial.print("Loaded settings from EEPROM\n");
  } else {
    Serial.print("EEPROM data not valid, reiniting\n");
    bindInitDefaults();
    bindWriteEeprom();
    Serial.print("EEPROM data saved\n");
  }

  setupPPMinput();

  attachInterrupt(IRQ_interrupt, RFM22B_Int, FALLING);

  init_rfm(0);
  rfmSetChannel(bind_data.hopchannel[RF_channel]);

  sei();

  digitalWrite(BUZZER, HIGH);
  digitalWrite(BTN, HIGH);
  Red_LED_ON ;
  delay(100);

  checkButton();

  Red_LED_OFF;
  digitalWrite(BUZZER, LOW);

  ppmAge = 255;
  rx_reset();

}


static uint8_t radioIDCounter = 0;
static RxToTxPacket recievePacket;


void loop(void)
{

  if (spiReadRegister(0x0C) == 0) {     // detect the locked module and reboot
    Serial.println("module locked?");
    Red_LED_ON;
    init_rfm(0);
    rx_reset();
    Red_LED_OFF;
  }

  if (RF_Mode == Received) {
    
    lastTelemetry = micros();
    RF_Mode = Receive;
    spiSendAddress(0x7f);   // Send the package read command

	uint8_t *buf = (uint8_t *)&recievePacket;
	for (uint8_t i = 0; i < sizeof(recievePacket); i++)
	{
		buf[i] = spiReadData();
	}
	RSSI_remote = recievePacket.miscDataByte;

#if MAVLINK_INJECT == 1
	mavlink_message_t msg; 
	mavlink_status_t status;

	for (uint8_t i = 0; i < recievePacket.dataLength; i++)
	{ 
		Serial.write(recievePacket.data[i]);
		//trying to grab msg  
		if(mavlink_parse_char(MAVLINK_COMM_0, recievePacket.data[i], &msg, &status))
		{
			radioIDCounter++;
			if (radioIDCounter > 40)
			{
				MAVLink_report(RSSI_remote);
				radioIDCounter = 0;
				// Inject Mavlink radio modem status package.
			}

		}
	}
#else
	Serial.write(recievePacket.data, recievePacket.dataLength);
#endif

  }

  uint32_t time = micros();

  if ((time - lastSent) >= modem_params[bind_data.modem_params].interval) {
    lastSent = time;

    if (1) {
      uint8_t tx_buf[sizeof(TxToRxPacket)];
      ppmAge++;

      if (lastTelemetry) {
        if ((time - lastTelemetry) > modem_params[bind_data.modem_params].interval) {
          // telemetry lost
          digitalWrite(BUZZER, HIGH);   // Buzzer on
          lastTelemetry=0;
        } else {
          // telemetry link re-established
          digitalWrite(BUZZER, LOW);   // Buzzer off
        }
      }

      // Construct packet to be sent
      if (FSstate == 2) {
        tx_buf[0] = 0xF5; // save failsafe
        Red_LED_ON
      } else {
        tx_buf[0] = 0x5E; // servo positions
        Red_LED_OFF

      }

	  TxToRxPacket *packet = (TxToRxPacket *)&tx_buf;

	  cli(); // disable interrupts when copying servo positions, to avoid race on 2 byte variable
      tx_buf[1] = (PPM[0] & 0xff);
      tx_buf[2] = (PPM[1] & 0xff);
      tx_buf[3] = (PPM[2] & 0xff);
      tx_buf[4] = (PPM[3] & 0xff);
      tx_buf[5] = ((PPM[0] >> 8) & 3) | (((PPM[1] >> 8) & 3) << 2) | (((PPM[2] >> 8) & 3) << 4) | (((PPM[3] >> 8) & 3) << 6);
      tx_buf[6] = (PPM[4] & 0xff);
      tx_buf[7] = (PPM[5] & 0xff);
      tx_buf[8] = (PPM[6] & 0xff);
      tx_buf[9] = (PPM[7] & 0xff);
      tx_buf[10] = ((PPM[4] >> 8) & 3) | (((PPM[5] >> 8) & 3) << 2) | (((PPM[6] >> 8) & 3) << 4) | (((PPM[7] >> 8) & 3) << 6);
      sei();

	  // Fill telemetry portion of packet
	  packet->dataLength = getSerialData(packet->data, sizeof(packet->data));

      //Green LED will be on during transmission
      Green_LED_ON ;

      // Send the data over RF
      rfmSetChannel(bind_data.hopchannel[RF_channel]);
      tx_packet(tx_buf, sizeof(tx_buf));

      //Hop to the next frequency
      RF_channel++;

      if (RF_channel >= bind_data.hopcount) {
        RF_channel = 0;
      }

      // do not switch channel as we may receive telemetry on the old channel
      if (modem_params[bind_data.modem_params].flags & 0x01) {
        RF_Mode = Receive;
        rx_reset();
      }

    } else {
      if (ppmAge == 8) {
        Red_LED_ON
      }

      ppmAge = 9;
      // PPM data outdated - do not send packets
    }

  }

  //Green LED will be OFF
  Green_LED_OFF;

  checkFS();
}
