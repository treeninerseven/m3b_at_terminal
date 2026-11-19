#pragma once
#include <stdint.h>
#include <string.h>
struct FakeEEPROM {
  uint8_t data[1024] = {};
  template<class T> void get(unsigned at, T &value) { memcpy(&value, data + at, sizeof(T)); }
  template<class T> void put(unsigned at, const T &value) { memcpy(data + at, &value, sizeof(T)); }
};
extern FakeEEPROM EEPROM;
