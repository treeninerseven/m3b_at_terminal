#pragma once
#include <stdint.h>

// Public laboratory values, NOT credentials for a deployed network.
namespace TestConfig {
constexpr uint32_t rfBaud = 921600;
constexpr uint32_t commandTimeoutMs = 5000;
constexpr uint32_t radioTimeoutMs = 60000;
constexpr uint8_t networkKey[16] = {
  0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
  0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
};
constexpr uint8_t shortAddress[2] = {0x12, 0x34};
constexpr uint8_t subnet[8] = {0xFD, 0, 0, 0, 0, 0, 0, 0};
constexpr int32_t profile = -1; // Reapply the queried profile. 0..7 explicitly selects a test profile.
constexpr bool gatewayAvailable = false;
constexpr bool autoRun = true;
}
