/******************************************************************************
  registers.h
  Fischer Moseley @ SparkFun Electronics
  Original Creation Date: July 31, 2019

  This file defines the memoryMap struct, which acts as the pseudo register map
  of the Qwiic Button/Switch. It also serves as an easy way to access variables
  and manipulate the state of the device.

  During I2C transactions, the memoryMap object is wrapped as a collection of
  bytes. The byte that the user is interested in (either to read or write) is
  selected with a register pointer. For instance, if the user sets the pointer
  to 0x0e, they will be addressing the 4th uint8_t sized object in this struct.
  In this case, that would be the interruptConfig register!

  This code is beerware; if you see me (or any other SparkFun employee) at the
  local, and you've found our code helpful, please buy us a round!

  Distributed as-is; no warranty is given.
******************************************************************************/

typedef union {
  struct {
    bool clickedEnable : 1; //This is bit 0. user mutable, set to 1 to enable an interrupt when the button is clicked. Defaults to 0.
    bool pressedEnable : 1; //user mutable, set to 1 to enable an interrupt when the button is pressed. Defaults to 0.
    bool: 6;
  };
  uint8_t byteWrapped;
} interruptConfigBitField;

typedef struct memoryMap {
  //Button Status/Configuration                       Register Address
  uint8_t id;                                             // 0x00
  uint8_t firmwareMinor;                                  // 0x01
  uint8_t firmwareMajor;                                  // 0x02

  uint8_t pressedStatus;                                  // 0x03
  uint8_t clickedStatus;                                  // 0x04

  //Interrupt Configuration
  interruptConfigBitField interruptConfigure;             // 0x05
  uint16_t buttonDebounceTime;                            // 0x06

  //Device Configuration
  uint8_t i2cAddress;                                     // 0x1F
};
