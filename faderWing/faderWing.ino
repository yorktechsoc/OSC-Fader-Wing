// Copyright (c) 2017 Electronic Theatre Controls, Inc., http://www.etcconnect.com
// Copyright (c) 2019 Stefan Staub
// Copyright (c) 2021 James Bithell, https://jbithell.com
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

/*
fader is a linear 10kOhm, from Bourns or ALPS and can be 45/60/100mm long
put 10nF ceramic capitors between ground and fader levelers to prevent analog noise
Arduino UNO, MEGA:
use IOREF +5V to the top (single pin) of the fader (100%)
GND to the center button pin (2 pins, the outer pin is normaly for the leveler) of the fader (0%)
TEENSY:
+3.3V to the top (single pin) of the fader (100%)
use ANALOG GND instead the normal GND to the center button pin (2 pins, the outer pin is normaly for the leveler) of the fader (0%)

put 100nF ceramic capitors between ground and the input of the buttons
*/

// libraries included
#include "Arduino.h"
#include <OSCMessage.h>

#ifdef BOARD_HAS_USB_SERIAL
	#include <SLIPEncodedUSBSerial.h>
	SLIPEncodedUSBSerial SLIPSerial(thisBoardsSerialUSB);
	#else
	#include <SLIPEncodedSerial.h>
	SLIPEncodedSerial SLIPSerial(Serial);
#endif

#include <LiquidCrystal_I2C.h>
LiquidCrystal_I2C lcd(0x27, 16, 2);

// PINs setup
#define POWER_LED 13
#define USB_LED 11
#define EOS_LED 12

static const int MOTOR_PINS[] = {5,23, 3,2,  6,7,  9,8,  10,15}; // static const int MOTOR_PINS[] = {22,23, 3,2,  19,18,  17,16,  14,15};

#define FADER_1_LEVELER			5
#define FADER_1_BUTTON	30
#define FADER_1_MOTORUP 5 // 22 -> 5
#define FADER_1_MOTORDOWN 23 // 23 (staying as 23)
#define FADER_2_LEVELER			3
#define FADER_2_BUTTON	31
#define FADER_2_MOTORUP 3
#define FADER_2_MOTORDOWN 2
#define FADER_3_LEVELER			4
#define FADER_3_BUTTON	29
#define FADER_3_MOTORUP 6 // 19 -> 6
#define FADER_3_MOTORDOWN 7 // 18 -> 7
#define FADER_4_LEVELER			2
#define FADER_4_BUTTON	28
#define FADER_4_MOTORUP 9 // 17 -> 9 
#define FADER_4_MOTORDOWN 8 // 16 -> 8
#define FADER_5_LEVELER			1
#define FADER_5_BUTTON	 26
#define FADER_5_MOTORUP 10 // 14 -> 10
#define FADER_5_MOTORDOWN 15 // 15 -> 15

#define MENU_UP_BUTTON  25
#define MENU_ENT_BUTTON  24
#define MENU_DOWN_BUTTON  27


// constants and macros
#define SUBSCRIBE		1
#define UNSUBSCRIBE	0

#define EDGE_DOWN		1
#define EDGE_UP			0

#define PING_AFTER_IDLE_INTERVAL		2500
#define TIMEOUT_AFTER_IDLE_INTERVAL	5000

int FADER_PAGE = 1; //Page to start on
bool FIVE_OFFSET = false; 
#define FADER_BANK				1 //Only comes in when you have lots of fader wings
#define NUMBER_OF_FADERS	10 // size of the faders per page on EOS / NOmad

#define FADER_UPDATE_RATE_MS	40 // update each 40ms
#define BUTTON_DEBOUNCE_RATE_MS 50

#define FADER_ACCURACY 30 //% 3
#define MOTOR_ACCURACY 20 //Out of 1024

uint32_t updateTime; 

const String HANDSHAKE_QUERY = "ETCOSC?";
const String HANDSHAKE_REPLY = "OK";
const String PING_QUERY = "faderwing_hello";
const String EOS_FADER = "/eos/fader";

// variables
bool connectedToEos = false;
unsigned long lastMessageRxTime = 0;
bool timeoutPingSent = false;

// datatypes
struct Fader {
	uint8_t number;
	uint8_t analogPin;
	uint8_t btnPin;
  uint8_t motorUpPin;
  uint8_t motorDownPin;
  bool moving;
  int16_t movingTarget;
  int16_t fiveOffsetEOSPos;
  int16_t EOSPos;
  int16_t faderMin;
  int16_t faderMax;
  int16_t faderResolution;
	int16_t analogLast;
	int16_t btnLast;
  unsigned long motorStartTime;
	String analogPattern;
	String btnPattern;
	uint32_t updateTime;
  bool sending;
	} fader1, fader2, fader3, fader4, fader5;
struct Button {
  uint8_t btnPin;
  int16_t btnLast;
  uint32_t updateTime;
  } upBtn, entBtn, downBtn;

/**
 * @brief send a ping with a message to the console
 *
 */
void sendPing() {
	OSCMessage ping("/eos/ping");
	ping.add(PING_QUERY.c_str());
	SLIPSerial.beginPacket();
	ping.send(SLIPSerial);
	SLIPSerial.endPacket();
	timeoutPingSent = true;
	}

/**
 * @brief 
 * add a filter so we don't get spammed with unwanted OSC messages from Eos
 * 
 */
void issueFilters() {
	OSCMessage filter("/eos/filter/add");
	filter.add("/eos/out/ping");
  filter.add("/eos/fader/*");
	SLIPSerial.beginPacket();
	filter.send(SLIPSerial);
	SLIPSerial.endPacket();

}

/**
 * @brief initialize a fader bank
 * 
 * @param bank number of the fader bank
 * @param page number of the fader page
 * @param faders number of faders in this bank
 */
void initFaders(uint8_t page) {
	String faderInit = "/eos/fader/";
	faderInit += FADER_BANK;
	faderInit += "/config/";
	faderInit += page;
	faderInit += '/';
	faderInit += NUMBER_OF_FADERS;
	OSCMessage faderBank(faderInit.c_str());
	SLIPSerial.beginPacket();
	faderBank.send(SLIPSerial);
	SLIPSerial.endPacket();
}

/**
 * @brief
 * Init the console, gives back a handshake reply
 * and send the subscribtions.
 *
 */
void initEOS() {
	// do the handshake reply for usb conection
	SLIPSerial.beginPacket();
	SLIPSerial.write((const uint8_t*)HANDSHAKE_REPLY.c_str(), (size_t)HANDSHAKE_REPLY.length());
	SLIPSerial.endPacket();

	// Let Eos know we want updates on some things
	issueFilters();

	// activate a fader bank
  if (connectedToEos) {
    changeLayer(FADER_PAGE,FIVE_OFFSET,&fader1,&fader2,&fader3,&fader4,&fader5);
  }
}

/**
 * @brief
 * Given an unknown OSC message we check to see if it's a handshake message.
 * If it's a handshake we issue a subscribe, otherwise we begin route the OSC
 * message to the appropriate function.
 *
 * @param msg OSC message
 */
void sendFader(struct Fader* fader, float pos,bool fiveOffset) {
    if (fader->moving){
      //Stop moving
      digitalWrite(fader->motorUpPin,LOW);
      digitalWrite(fader->motorDownPin,LOW);
      delay(5);
      fader->moving = false;
    }

    if (pos <= 0.01){
      pos = fader->faderMin/fader->faderResolution;
    } else if (pos >= 0.99){
      pos = fader->faderMax/fader->faderResolution;
    }
    if (fiveOffset == FIVE_OFFSET) {
        fader->moving = true;
        motorGoTo(fader,int(pos*fader->faderResolution));
    }
    if (fiveOffset) {
      fader->fiveOffsetEOSPos = int(pos*fader->faderResolution);
      } else {
      fader->EOSPos = int(pos*fader->faderResolution);
      }
}

void parseFaderUpdate1(OSCMessage& msg, int addressOffset) {
  sendFader(&fader1,msg.getOSCData(0)->getFloat(),false);
}
void parseFaderUpdate2(OSCMessage& msg, int addressOffset) {
  sendFader(&fader2,msg.getOSCData(0)->getFloat(),false);
}
void parseFaderUpdate3(OSCMessage& msg, int addressOffset) {
  sendFader(&fader3,msg.getOSCData(0)->getFloat(),false);
}
void parseFaderUpdate4(OSCMessage& msg, int addressOffset) {
  sendFader(&fader4,msg.getOSCData(0)->getFloat(),false);
}
void parseFaderUpdate5(OSCMessage& msg, int addressOffset) {
  sendFader(&fader5,msg.getOSCData(0)->getFloat(),false);
}
void parseFaderUpdateO1(OSCMessage& msg, int addressOffset) {
  sendFader(&fader1,msg.getOSCData(0)->getFloat(),true);
}
void parseFaderUpdateO2(OSCMessage& msg, int addressOffset) {
  sendFader(&fader2,msg.getOSCData(0)->getFloat(),true);
}
void parseFaderUpdateO3(OSCMessage& msg, int addressOffset) {
  sendFader(&fader3,msg.getOSCData(0)->getFloat(),true);
}
void parseFaderUpdateO4(OSCMessage& msg, int addressOffset) {
  sendFader(&fader4,msg.getOSCData(0)->getFloat(),true);
}
void parseFaderUpdateO5(OSCMessage& msg, int addressOffset) {
  sendFader(&fader5,msg.getOSCData(0)->getFloat(),true);

}
void parseOSCMessage(String& msg) {
	// check to see if this is the handshake string
	if (msg.indexOf(HANDSHAKE_QUERY) != -1) {
		// handshake string found!
		connectedToEos = true;
    digitalWrite(EOS_LED,HIGH);
		initEOS();
		}

	if (msg.indexOf(PING_QUERY) != -1) {
		// handshake string found!
		connectedToEos = true;
    digitalWrite(EOS_LED,HIGH);
	}

  // prepare the message for routing by filling an OSCMessage object with our message string
  OSCMessage oscmsg;
  oscmsg.fill((uint8_t*)msg.c_str(), (int)msg.length());
  oscmsg.route("/eos/fader/1/1", parseFaderUpdate1); //Needs changing to String("/eos/fader/" + String(FADER_BANK) + "/1")
  oscmsg.route("/eos/fader/1/2", parseFaderUpdate2);
  oscmsg.route("/eos/fader/1/3", parseFaderUpdate3);
  oscmsg.route("/eos/fader/1/4", parseFaderUpdate4);
  oscmsg.route("/eos/fader/1/5", parseFaderUpdate5);
  oscmsg.route("/eos/fader/1/6", parseFaderUpdateO1);
  oscmsg.route("/eos/fader/1/7", parseFaderUpdateO2);
  oscmsg.route("/eos/fader/1/8", parseFaderUpdateO3);
  oscmsg.route("/eos/fader/1/9", parseFaderUpdateO4);
  oscmsg.route("/eos/fader/1/10", parseFaderUpdateO5);
}

/**
 * @brief initialise the fader
 *
 * @param fader
 * @param bank
 * @param number
 * @param analogPin
 * @param btnPin
 */
void initFader(struct Fader* fader, uint8_t number, uint8_t analogPin, uint8_t btnPin, uint8_t motorUpPin, uint8_t motorDownPin) {
	fader->number = number;
	fader->analogPin = analogPin;
	fader->btnPin = btnPin;
  fader->motorUpPin = motorUpPin;
  fader->motorDownPin = motorDownPin;
  fader->moving = false;
  fader->sending = false;
  fader->movingTarget = 512;
  fader->fiveOffsetEOSPos = 0;
  fader->EOSPos = 0;
  fader->motorStartTime = 0;
  fader->faderMin = 0;
  fader->faderMax = 1024;
  //Calibrate Fader
  digitalWrite(motorUpPin,HIGH);
  digitalWrite(motorDownPin,LOW); 
  delay(500);
  digitalWrite(motorUpPin, LOW);
  digitalWrite(motorDownPin,LOW);
  fader->faderMax = analogRead(analogPin);
  delay(25);
  digitalWrite(motorUpPin, LOW);
  digitalWrite(motorDownPin,HIGH);
  delay(500);
  digitalWrite(motorUpPin, LOW);
  digitalWrite(motorDownPin,LOW);
  fader->faderMin = analogRead(analogPin);
  fader->faderResolution = fader->faderMax - fader->faderMin;
	fader->analogLast = 0xFFFF; // forces an osc output of the fader
	pinMode(fader->btnPin, INPUT_PULLUP);
  fader->btnLast = digitalRead(fader->btnPin);
	fader->analogPattern = EOS_FADER + '/' + String(FADER_BANK) + '/' + String(fader->number);
	fader->btnPattern = EOS_FADER + '/' + String(FADER_BANK) + '/' + String(fader->number) + "/fire"; //or /stop
	fader->updateTime = millis();
}
void initButton(struct Button* button, uint8_t btnPin) {
  button->btnPin = btnPin;
  pinMode(button->btnPin, INPUT_PULLUP);
  button->btnLast = digitalRead(button->btnPin);
  button->updateTime = millis();
}

void resetFader(struct Fader* fader){
  fader->sending = false;
  digitalWrite(fader->motorUpPin,LOW);
  digitalWrite(fader->motorDownPin,LOW);
  fader->moving = true; 
}
/*
 * Change the layer the buttons are on
 */
void changeLayer(uint8_t newPage, bool fiveOffset, struct Fader* fader1,struct Fader* fader2,struct Fader* fader3,struct Fader* fader4,struct Fader* fader5) {
    FADER_PAGE = newPage;
    FIVE_OFFSET = fiveOffset;
    resetFader(fader1);
    resetFader(fader2);
    resetFader(fader3);
    resetFader(fader4);
    resetFader(fader5);

    fader1->number = (fiveOffset ? 6 : 1);
    fader1->analogPattern = EOS_FADER + '/' + String(FADER_BANK) + '/' + String(fader1->number);
    fader1->btnPattern = EOS_FADER + '/' + String(FADER_BANK) + '/' + String(fader1->number) + "/fire";
    motorGoTo(fader1,(fiveOffset ? fader1->fiveOffsetEOSPos : fader1->EOSPos));
    fader2->number = (fiveOffset ? 7 : 2);
    fader2->analogPattern = EOS_FADER + '/' + String(FADER_BANK) + '/' + String(fader2->number);
    fader2->btnPattern = EOS_FADER + '/' + String(FADER_BANK) + '/' + String(fader2->number) + "/fire";
    motorGoTo(fader2,(fiveOffset ? fader2->fiveOffsetEOSPos : fader2->EOSPos));
    fader3->number = (fiveOffset ? 8 : 3);
    fader3->analogPattern = EOS_FADER + '/' + String(FADER_BANK) + '/' + String(fader3->number);
    fader3->btnPattern = EOS_FADER + '/' + String(FADER_BANK) + '/' + String(fader3->number) + "/fire";
    motorGoTo(fader3,(fiveOffset ? fader3->fiveOffsetEOSPos : fader3->EOSPos));
    fader4->number = (fiveOffset ? 9 : 4);
    fader4->analogPattern = EOS_FADER + '/' + String(FADER_BANK) + '/' + String(fader4->number);
    fader4->btnPattern = EOS_FADER + '/' + String(FADER_BANK) + '/' + String(fader4->number) + "/fire";
    motorGoTo(fader4,(fiveOffset ? fader4->fiveOffsetEOSPos : fader4->EOSPos));
    fader5->number = (fiveOffset ? 10 : 5);
    fader5->analogPattern = EOS_FADER + '/' + String(FADER_BANK) + '/' + String(fader5->number);
    fader5->btnPattern = EOS_FADER + '/' + String(FADER_BANK) + '/' + String(fader5->number) + "/fire";
    motorGoTo(fader5,(fiveOffset ? fader5->fiveOffsetEOSPos : fader5->EOSPos));
    initFaders(newPage);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("EOS Fader Page");
    lcd.print(FADER_PAGE);
    lcd.setCursor(0, 1);
    if (fiveOffset) {
      lcd.print("6  7   8   9  10");
    } else {
      lcd.print("1  2   3   4   5");
    }
}

 /*
  * Send a motor to a given spot
  */
void motorGoTo(struct Fader* fader, int pos) {
  //Instruct the motor to go somewhere
    fader->moving = true;
    fader->motorStartTime = millis();
    if (pos < fader->faderMin) {
      pos = fader->faderMin+MOTOR_ACCURACY;
    } else if (pos > fader->faderMax) {
      pos = fader->faderMax-MOTOR_ACCURACY;
    }
    fader->movingTarget = pos; 
}
void moveMotor(struct Fader* fader) {
  //Actually move the motor in the loop
  if (fader->moving) {
    if (analogRead(fader->analogPin) == fader->faderMax || analogRead(fader->analogPin) == fader->faderMin) {
      //Stop moving
      digitalWrite(fader->motorUpPin,LOW);
      digitalWrite(fader->motorDownPin,LOW);
      delay(50); // ensure it's stopped
      fader->moving = false; 
      }
    else if (analogRead(fader->analogPin) < (fader->movingTarget+MOTOR_ACCURACY) && analogRead(fader->analogPin) > (fader->movingTarget-MOTOR_ACCURACY)) {
      //Stop moving
      digitalWrite(fader->motorUpPin,LOW);
      digitalWrite(fader->motorDownPin,LOW);
      delay(50); // ensure it's stopped
      fader->moving = false; 
      } 
    else if (analogRead(fader->analogPin) > (fader->movingTarget)) {
      digitalWrite(fader->motorUpPin,LOW);
      analogWrite(fader->motorDownPin, 150);
    } else if (analogRead(fader->analogPin) < (fader->movingTarget)) {
      analogWrite(fader->motorUpPin, 150);
      digitalWrite(fader->motorDownPin,LOW);
    }
    else if (millis() - fader->motorStartTime > 250){
      digitalWrite(fader->motorUpPin,LOW);
      digitalWrite(fader->motorDownPin,LOW);
      delay(50); // ensure it's stopped
      fader->moving = false; 
      updateFader(fader);
    }
  }
}
void updateFader(struct Fader* fader) {
  if (!fader->moving){
    if((fader->updateTime + FADER_UPDATE_RATE_MS) < millis()) {
      int16_t raw = analogRead(fader->analogPin);
      int16_t resolution = fader->faderResolution;
      if (raw < fader->faderMin){
        fader->faderMin = raw;
        fader->faderResolution = fader->faderMax - fader->faderMin;
      }
      if (raw > fader->faderMax){
        fader->faderMax = raw;
        fader->faderResolution = fader->faderMax - fader->faderMin;

      }
      if ((raw-FADER_ACCURACY >= fader->analogLast) || (raw+FADER_ACCURACY <= fader->analogLast)) {
        float value = ((((raw) * 1.0)) / resolution); // normalize to values between 0.0 and 1.0 
        fader->analogLast = raw;

        if (value > 0.97) value = (float) 1.0; //Normalise top values
        else if (value < 0.04) value = (float) 0.0; //Normalise low values
        fader->sending = true;

        OSCMessage faderUpdate(fader->analogPattern.c_str());
        faderUpdate.add(value);
        SLIPSerial.beginPacket();
        faderUpdate.send(SLIPSerial);
        SLIPSerial.endPacket();
      }

      if((digitalRead(fader->btnPin)) != fader->btnLast) {
        OSCMessage btnUpdate(fader->btnPattern.c_str());
        if(fader->btnLast == LOW) {
          fader->btnLast = HIGH;
          btnUpdate.add(EDGE_DOWN);
        }
        else {
          fader->btnLast = LOW;
          btnUpdate.add(EDGE_UP);
        }
        SLIPSerial.beginPacket();
        btnUpdate.send(SLIPSerial);
        SLIPSerial.endPacket();
      }

      fader->updateTime = millis();
    }
  }
}

void updateButton(struct Button* btn, int btnType) {
  if((btn->updateTime + BUTTON_DEBOUNCE_RATE_MS) < millis()) {
    if((digitalRead(btn->btnPin)) != btn->btnLast) {
      if(btn->btnLast == LOW) {
        btn->btnLast = HIGH;
        if (btnType == 1) { //Up
          if (FADER_PAGE > 1 || (FADER_PAGE == 1 && FIVE_OFFSET)) {
            changeLayer((FIVE_OFFSET ? FADER_PAGE : FADER_PAGE-1),!FIVE_OFFSET,&fader1,&fader2,&fader3,&fader4,&fader5);
          }
        } else if (btnType == 2) {
          changeLayer(1,false,&fader1,&fader2,&fader3,&fader4,&fader5);
        } else { //Down
          if (FADER_PAGE < 99 || (FADER_PAGE == 99 && !FIVE_OFFSET)) {
            changeLayer((FIVE_OFFSET ? FADER_PAGE+1 : FADER_PAGE),!FIVE_OFFSET,&fader1,&fader2,&fader3,&fader4,&fader5);
          }
        }        
      }
      else {
        btn->btnLast = LOW;
      }
    }
    btn->updateTime = millis();
  }
}

/**
 * @brief setup arduino
 *
 */
void setup() {
	SLIPSerial.begin(115200);

	#ifdef BOARD_HAS_USB_SERIAL
	 while (!SerialUSB);
	 #else
	 while (!Serial);
	#endif
	// this is necessary for reconnecting a device because it need some timme for the serial port to get open, but meanwhile the handshake message was send from eos

  //Setup LEDs
  pinMode(POWER_LED, OUTPUT); 
  digitalWrite(POWER_LED,HIGH);
  pinMode(USB_LED, OUTPUT); 
  digitalWrite(USB_LED,HIGH); 
  pinMode(EOS_LED, OUTPUT); 
  digitalWrite(EOS_LED,LOW);

  //Setup LCD
  lcd.begin();
  //lcd.noBacklight();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Starting Up.....");

  //Setup Motors
  for (int i=0; i<sizeof MOTOR_PINS/sizeof MOTOR_PINS[0]; i++) {
    //Make sure all motors powered off
    pinMode(MOTOR_PINS[i], OUTPUT); 
    digitalWrite(MOTOR_PINS[i],LOW);
  } 

  //Connect to EOS
	initEOS();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  Calibrating   ");
  lcd.setCursor(0, 1);
  lcd.print("!!HANDS  CLEAR!!");
	// init of hardware elements
	initFader(&fader1, 1, FADER_1_LEVELER, FADER_1_BUTTON, FADER_1_MOTORUP, FADER_1_MOTORDOWN);
	initFader(&fader2, 2, FADER_2_LEVELER, FADER_2_BUTTON, FADER_2_MOTORUP, FADER_2_MOTORDOWN);
	initFader(&fader3, 3, FADER_3_LEVELER, FADER_3_BUTTON, FADER_3_MOTORUP, FADER_3_MOTORDOWN);
	initFader(&fader4, 4, FADER_4_LEVELER, FADER_4_BUTTON, FADER_4_MOTORUP, FADER_4_MOTORDOWN);
	initFader(&fader5, 5, FADER_5_LEVELER, FADER_5_BUTTON, FADER_5_MOTORUP, FADER_5_MOTORDOWN);
  initButton(&upBtn, MENU_UP_BUTTON);
  initButton(&entBtn, MENU_ENT_BUTTON);
  initButton(&downBtn, MENU_DOWN_BUTTON);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("     Ready      ");
}

bool powerBlock = false; //Should the logic be blocked because of low power
void loop() {
  if (analogRead(0) < 1023) { //Needs a big old PSU at least 2A (ideally 9v) to deliver enough beans for the motors so double check that we have a PSU plugged in
    lcd.setCursor(0, 0);
    lcd.print(" Connect Power! ");
    lcd.setCursor(0, 1);
    lcd.print(analogRead(0));
    digitalWrite(POWER_LED,LOW);
    powerBlock = true;
    delay(500);
    return; //Block the rest of the logic
  } else if (powerBlock) {
    lcd.clear();
    digitalWrite(POWER_LED,HIGH);
    powerBlock = false;
  }
  
	static String curMsg;
	// Then we check to see if any OSC commands have come from Eos
	// and update the display accordingly.
	int size = SLIPSerial.available();
	if (size > 0) {
		while (size--) curMsg += (char)(SLIPSerial.read());
		}
	if (SLIPSerial.endofPacket()) {
    lastMessageRxTime = millis();
		parseOSCMessage(curMsg);
		// We only care about the ping if we haven't heard recently
		// Clear flag when we get any traffic
		timeoutPingSent = false;
		curMsg = String();
		}

	if(lastMessageRxTime > 0) {
		unsigned long diff = millis() - lastMessageRxTime;
		//We first check if it's been too long and we need to time out
		if(diff > TIMEOUT_AFTER_IDLE_INTERVAL) {
			connectedToEos = false;
      digitalWrite(EOS_LED,LOW);
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("No EOS Connected");
			lastMessageRxTime = 0;
			timeoutPingSent = false;
			}

		// It could be the console is sitting idle. Send a ping once to
		// double check that it's still there, but only once after 2.5s have passed
		if(!timeoutPingSent && diff > PING_AFTER_IDLE_INTERVAL) {
			sendPing();
			}
		}

	// do all necessary updates
	updateFader(&fader1);
	updateFader(&fader2);
	updateFader(&fader3);
	updateFader(&fader4);
	updateFader(&fader5);
  updateButton(&upBtn,1);
  updateButton(&entBtn,2);
  updateButton(&downBtn,3);
  moveMotor(&fader1);
  moveMotor(&fader2);
  moveMotor(&fader3);
  moveMotor(&fader4);
  moveMotor(&fader5);
}