#pragma once

#include <Arduino_DriveBus_Library.h>
#include <Wire.h>
#include "pin_config.h"

class BoardTouch {
 public:
  const char *begin() {
    if (address == FT3168_DEVICE_ADDRESS) {
      detachInterrupt(TP_INT);
    }
    address = 0;
    ftPending = false;
    if (!Wire.begin(IIC_SDA, IIC_SCL)) {
      return nullptr;
    }

    // Wake a sleeping CST820B before probing either panel revision.
    pinMode(TP_RESET, OUTPUT);
    digitalWrite(TP_RESET, LOW);
    delay(20);
    digitalWrite(TP_RESET, HIGH);
    delay(120);

    auto bus = std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
    Wire.beginTransmission(CST816D_DEVICE_ADDRESS);
    if (Wire.endTransmission() == 0) {
      Arduino_CST816x driver(bus, CST816D_DEVICE_ADDRESS);
      if (driver.begin()) {
        address = CST816D_DEVICE_ADDRESS;
        return "CST820B (0x15)";
      }
      return nullptr;
    }

    Wire.beginTransmission(FT3168_DEVICE_ADDRESS);
    if (Wire.endTransmission() == 0) {
      Arduino_FT3x68 driver(bus, FT3168_DEVICE_ADDRESS);
      if (driver.begin()) {
        pinMode(TP_INT, INPUT_PULLUP);
        attachInterruptArg(TP_INT, [](void *arg) {
          static_cast<BoardTouch *>(arg)->ftPending = true;
        }, this, FALLING);
        address = FT3168_DEVICE_ADDRESS;
        return "FT6146 (0x38)";
      }
    }
    return nullptr;
  }

  bool readPoint(int16_t &x, int16_t &y) {
    if (address == 0 || (address == FT3168_DEVICE_ADDRESS && !ftPending)) {
      return false;
    }
    ftPending = false;

    // Both controllers report the contact count and the first point at 0x02-0x06.
    constexpr size_t reportSize = 5;
    Wire.beginTransmission(address);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(address, reportSize) != reportSize) {
      return false;
    }
    uint8_t report[reportSize];
    if (Wire.readBytes(report, reportSize) != reportSize) {
      return false;
    }
    const uint8_t contacts = report[0] & 0x0F;
    const uint8_t event = report[1] >> 6;  // 0: down, 1: up, 2: contact, 3: none
    if (contacts == 0 || contacts > 2 || event == 1 || event == 3) {
      return false;
    }
    x = ((report[1] & 0x0F) << 8) | report[2];
    y = ((report[3] & 0x0F) << 8) | report[4];
    return true;
  }

 private:
  uint8_t address = 0;
  volatile bool ftPending = false;
};
