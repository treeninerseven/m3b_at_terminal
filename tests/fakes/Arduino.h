#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define DATA_EEPROM_BASE 1
#define USART_ISR_TC 1
struct FakeRegisters { uint32_t ISR = USART_ISR_TC; };
struct UART_HandleTypeDef { FakeRegisters *Instance; };
struct Uart {
  bool usb = false;
  Uart() = default;
  explicit Uart(bool isUsb) : usb(isUsb) {}
  Uart(int, int) {}
  void begin(uint32_t, int = 0) {}
  void dtr(bool) {}
  explicit operator bool() const { return true; }
  int available();
  int read();
  int availableForWrite() { return 64; }
  size_t write(const uint8_t *, size_t);
  UART_HandleTypeDef *getHandle();
};
extern Uart SerialUSB;
uint32_t millis();
inline uint32_t HAL_GetUIDw0() { return 0x11223344; }
inline uint32_t HAL_GetUIDw1() { return 0x55667788; }
inline uint32_t HAL_GetUIDw2() { return 0x99AABBCC; }
