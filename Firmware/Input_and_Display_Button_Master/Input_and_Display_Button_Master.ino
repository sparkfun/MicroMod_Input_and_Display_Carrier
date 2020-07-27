/*
  An I2C based Button
  By: Nathan Seidle and Fischer Moseley and Priyanka Makin
  SparkFun Electronics
  Date: July 31st, 2019
  License: This code is public domain but you buy me a beer if you use this and
  we meet someday (Beerware license).

  Qwiic Button is an I2C based button that records any button presses to a queue.

  Qwiic Button maintains a queue of events. To remove events from the queue write
  the appropriate register (timeSinceLastButtonClicked or timeSinceLastButtonPressed)
  to zero. The register will then be filled with the next available event time.

  There is also an accompanying Arduino Library located here:
  https://github.com/sparkfun/SparkFun_Qwiic_Button_Arduino_Library

  Feel like supporting our work? Buy a board from SparkFun!
  https://www.sparkfun.com/products/14641

  To install support for ATtiny84 in Arduino IDE: https://github.com/SpenceKonde/ATTinyCore/blob/master/Installation.md
  This core is installed from the Board Manager menu
  This core has built in support for I2C S/M and serial
  If you have Dave Mellis' ATtiny installed you may need to remove it from \Users\xx\AppData\Local\Arduino15\Packages

  To support 400kHz I2C communication reliably ATtiny84 needs to run at 8MHz. This requires user to
  click on 'Burn Bootloader' before code is loaded.

  Library Inclusion:
  Wire.h        Used for interfacing with the I2C hardware for responding to I2C events.
  EEPROM.h      Used for interfacing with the onboard EEPROM for storing and retrieving settings.
  nvm.h         Used for defining the storage locations in non-volatile memory (EEPROM) to store and retrieve settings from.
  queue.h       Used for defining a FIFO-queue that contains the timestamps associated with pressing and clicking the button.
  registers.h   Used for defining a memoryMap object that serves as the register map for the device.
  led.h         Used for configuring the behavior of the onboard LED (in the case of the Qwiic Button)
                  or the offboard LED (in the case of the Qwiic Switch)

  PinChangeInterrupt.h    Nico Hoo's library for triggering an interrupt on a pin state change (either low->high or high->low)
  avr/sleep.h             Needed for sleep_mode which saves power on the ATTiny
  avr/power.hardware      Needed for powering down peripherals such as the ADC/TWI and Timers on the ATTiny
*/

#include <Wire.h>
#include <EEPROM.h>
#include "nvm.h"
#include "registers.h"

#include "PinChangeInterrupt.h" //Nico Hood's library: https://github.com/NicoHood/PinChangeInterrupt/
//Used for pin change interrupts on ATtinys (encoder button causes interrupt)
//Note: To make this code work with Nico's library you have to comment out https://github.com/NicoHood/PinChangeInterrupt/blob/master/src/PinChangeInterruptSettings.h#L228

#include <avr/sleep.h> //Needed for sleep_mode
#include <avr/power.h> //Needed for powering down perihperals such as the ADC/TWI and Timers

#define DEVICE_ID 0xAE
#define FIRMWARE_MAJOR 0x01 //Firmware Version. Helpful for tech support.
#define FIRMWARE_MINOR 0x00

#define DEFAULT_I2C_ADDRESS 0x71

#define SOFTWARE_ADDRESS true
#define HARDWARE_ADDRESS false

uint8_t oldAddress;

#define NUM_BUTTONS             7
#define EVENT_AVAILABLE         7 //Position of the EVENT_AVAILABLE bit
#define BUTTON_PIN_A            10
#define BUTTON_PIN_B            9
#define BUTTON_PIN_UP           2
#define BUTTON_PIN_DOWN         0
#define BUTTON_PIN_LEFT         8
#define BUTTON_PIN_RIGHT        1
#define BUTTON_PIN_CENTER       3

enum buttons {
  A,
  B,
  UP,
  DOWN,
  LEFT,
  RIGHT,
  CENTER
};

//Hardware connections
#if defined(__AVR_ATmega328P__)
//For developement on an Uno
const uint8_t addressPin0 = 3;
const uint8_t addressPin1 = 4;
const uint8_t addressPin2 = 5;
const uint8_t addressPin3 = 6;
const uint8_t ledPin = 9; //PWM
const uint8_t statusLedPin = 8;
const uint8_t switchPin = 2;
const uint8_t interruptPin = 7; //pin is active-low, high-impedance when not triggered, goes low-impedance to ground when triggered

#elif defined(__AVR_ATtiny84__)
const uint8_t buttonPin[NUM_BUTTONS] = {BUTTON_PIN_A, BUTTON_PIN_B, BUTTON_PIN_UP, BUTTON_PIN_DOWN, BUTTON_PIN_LEFT, BUTTON_PIN_RIGHT, BUTTON_PIN_CENTER};
const uint8_t interruptPin = 7; //pin is active-low, high-impedance when not triggered, goes low-impedance to ground when triggered
#endif

//Global variables
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
//These are the defaults for all settings

//Variables used in the I2C interrupt.ino file so we use volatile
volatile memoryMap registerMap {
  DEVICE_ID,                //id
  FIRMWARE_MINOR,           //firmwareMinor
  FIRMWARE_MAJOR,           //firmwareMajor
  0x00,                     //pressedStatus {EventAvailable, center, right, left, down, up, b, a}
  0x00,                     //clickedStatus {EventAvailable, center, right, left, down, up, b, a}
  {1, 1},                   //interruptConfig {pressedEnable, clickedEnable}
  0x000A,                   //buttonDebounceTime
  DEFAULT_I2C_ADDRESS,      //i2cAddress
};

//This defines which of the registers are read-only (0) vs read-write (1)
memoryMap protectionMap = {
  0x00,       //id
  0x00,       //firmwareMinor
  0x00,       //firmwareMajor
  0x00,       //clickedStatus {A(MSB), B, Up, Down, Left, Right, Center, eventAvailable (LSB)}
  0x00,       //pressedStatus {A(MSB), B, Up, Down, Left, Right, Center, eventAvailable (LSB)}
  {1, 1},     //interruptConfig {pressedEnable, clickedEnable}
  0xFFFF,     //buttonDebounceTime
  0xFF,       //i2cAddress
};

//Cast 32bit address of the object registerMap with uint8_t so we can increment the pointer
uint8_t *registerPointer = (uint8_t *)&registerMap;
uint8_t *protectionPointer = (uint8_t *)&protectionMap;

volatile uint8_t registerNumber; //Gets set when user writes an address. We then serve the spot the user requested.

volatile boolean updateFlag = true; //Goes true when we receive new bytes from user. Causes LEDs and things to update in main loop.

volatile unsigned long lastClickTime = 0; //Used for debouncing

//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

void setup(void)
{
  //Enable Pullups on all buttons
  for (int buttonNum = 0; buttonNum < NUM_BUTTONS; buttonNum++)
  {
    pinMode(buttonPin[buttonNum], INPUT_PULLUP); //GPIO with internal pullup, goes low when button is pushed
  }

#if defined(__AVR_ATmega328P__)
  pinMode(interruptPin, INPUT_PULLUP);     //High-impedance input until we have an int and then we output low. Pulled high with 10k with cuttable jumper.
#else
  pinMode(interruptPin, INPUT_PULLUP);     //High-impedance input until we have an int and then we output low. Pulled high with 10k with cuttable jumper.
#endif

  //Disable ADC
  ADCSRA = 0;

  //Disable Brown-Out Detect
  MCUCR = bit(BODS) | bit(BODSE);
  MCUCR = bit(BODS);

  //Power down various bits of hardware to lower power usage
  //set_sleep_mode(SLEEP_MODE_PWR_DOWN); //May turn off millis
  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();

#if defined(__AVR_ATmega328P__)

  for (int x = 0; x < 100; x++) {
    EEPROM.put(x, 0xFF);
  }

  Serial.begin(115200);
  Serial.println("Qwiic Button");
  Serial.print("Address: 0x");
  Serial.println(registerMap.i2cAddress, HEX);
  Serial.print("Device ID: 0x");
  Serial.println(registerMap.id, HEX);

#endif

  //readSystemSettings(&registerMap); //Load all system settings from EEPROM

#if defined(__AVR_ATmega328P__)

#endif

  setupInterrupts();               //Enable pin change interrupts for I2C, switch, etc
  startI2C(&registerMap);          //Determine the I2C address we should be using and begin listening on I2C bus
  oldAddress = registerMap.i2cAddress;

}

void loop(void)
{
  //Check to see if the I2C Address has been updated by software, set the appropriate address type flag
  if (oldAddress != registerMap.i2cAddress)
  {
    oldAddress = registerMap.i2cAddress;
    EEPROM.put(LOCATION_ADDRESS_TYPE, SOFTWARE_ADDRESS);
  }

  //update interruptPin output
  if ((((registerMap.pressedStatus & (1 << EVENT_AVAILABLE)) >> EVENT_AVAILABLE) && registerMap.interruptConfigure.pressedEnable) ||
      (((registerMap.clickedStatus & (1 << EVENT_AVAILABLE)) >> EVENT_AVAILABLE) && registerMap.interruptConfigure.clickedEnable))
  { //if the interrupt is triggered
    pinMode(interruptPin, OUTPUT); //make the interrupt pin a low-impedance connection to ground
    digitalWrite(interruptPin, LOW);
  }
  else
  { //go to high-impedance mode on the interrupt pin if the interrupt is not triggered
#if defined(__AVR_ATmega328P__)
    pinMode(interruptPin, INPUT_PULLUP);
#else
    pinMode(interruptPin, INPUT_PULLUP);
#endif
  }

  if (updateFlag == true)
  {
    //Record anything new to EEPROM (like new LED values)
    //It can take ~3.4ms to write a byte to EEPROM so we do that here instead of in an interrupt
    recordSystemSettings(&registerMap);

    //Calculate LED values based on pulse settings if anything has changed

    updateFlag = false; //clear flag
  }

  sleep_mode();             //Stop everything and go to sleep. Wake up if I2C event occurs.
}

//Update slave I2C address to what's configured with registerMap.i2cAddress and/or the address jumpers.
void startI2C(memoryMap *map)
{
  uint8_t address;
  //if the value is legal, then set it
  if (map->i2cAddress > 0x07 && map->i2cAddress < 0x78)
    address = map->i2cAddress;

  //if the value is illegal, default to the default I2C address for our platform
  else
    address = DEFAULT_I2C_ADDRESS;

  //save new address to the register map
  map->i2cAddress = address;

  //reconfigure Wire instance
  Wire.end();          //stop I2C on old address
  Wire.begin(address); //rejoin the I2C bus on new address

  //The connections to the interrupts are severed when a Wire.begin occurs, so here we reattach them
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);
}

//Reads the current system settings from EEPROM
//If anything looks weird, reset setting to default value
void readSystemSettings(memoryMap *map)
{
  //Read what I2C address we should use
  EEPROM.get(LOCATION_I2C_ADDRESS, map->i2cAddress);
  if (map->i2cAddress == 255)
  {
    map->i2cAddress = DEFAULT_I2C_ADDRESS; //By default, we listen for DEFAULT_I2C_ADDRESS
    EEPROM.put(LOCATION_I2C_ADDRESS, map->i2cAddress);
  }

  //Error check I2C address we read from EEPROM
  if (map->i2cAddress < 0x08 || map->i2cAddress > 0x77)
  {
    //User has set the address out of range
    //Go back to defaults
    map->i2cAddress = DEFAULT_I2C_ADDRESS;
    EEPROM.put(LOCATION_I2C_ADDRESS, map->i2cAddress);
  }

  //Read the interrupt bits
  EEPROM.get(LOCATION_INTERRUPTS, map->interruptConfigure.byteWrapped);
  if (map->interruptConfigure.byteWrapped == 0xFF)
  {
    map->interruptConfigure.byteWrapped = 0x03; //By default, enable the click and pressed interrupts
    EEPROM.put(LOCATION_INTERRUPTS, map->interruptConfigure.byteWrapped);
  }

  EEPROM.get(LOCATION_BUTTON_DEBOUNCE_TIME, map->buttonDebounceTime);
  if (map->buttonDebounceTime == 0xFFFF)
  {
    map->buttonDebounceTime = 10; //Default to 10ms
    EEPROM.put(LOCATION_BUTTON_DEBOUNCE_TIME, map->buttonDebounceTime);
  }

}

//If the current setting is different from that in EEPROM, update EEPROM
void recordSystemSettings(memoryMap *map)
{
  //I2C address is byte
  byte i2cAddr;

  //Error check the current I2C address
  if (map->i2cAddress >= 0x08 && map->i2cAddress <= 0x77)
  {
    //Address is valid

    //Read the value currently in EEPROM. If it's different from the memory map then record the memory map value to EEPROM.
    EEPROM.get(LOCATION_I2C_ADDRESS, i2cAddr);
    if (i2cAddr != map->i2cAddress)
    {
      EEPROM.put(LOCATION_I2C_ADDRESS, (byte)map->i2cAddress);
      startI2C(map); //Determine the I2C address we should be using and begin listening on I2C bus
    }
  }
  else
  {
    EEPROM.get(LOCATION_I2C_ADDRESS, i2cAddr);
    map->i2cAddress == i2cAddr; //Return to original address
  }


  EEPROM.put(LOCATION_INTERRUPTS, map->interruptConfigure.byteWrapped);
  EEPROM.put(LOCATION_BUTTON_DEBOUNCE_TIME, map->buttonDebounceTime);
}
