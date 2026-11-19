#pragma once
#include <stddef.h>
#include <stdint.h>
#include "RfDmaRx.h"

constexpr size_t LINE_SIZE = 1024, OUTPUT_SIZE = 4096;
struct RxChunk { uint32_t ms; uint16_t offset, size; };
struct TestCapture {
  uint8_t tx[600], rx[4096];
  RxChunk chunks[96];
  size_t txSize, rxSize, readCursor, chunkCount, apiRxSize;
  uint32_t started, apiEnded, deadline, txMs, lastRx, timeoutCount;
  uint32_t droppedRx;
  bool txOverflow, chunkOverflow, writeFailed;
  RfRxStats before;
};
// The terminal and blocking test runner never run together. Sharing this RAM
// preserves the original terminal capacities and leaves room for the C client's
// response buffers and variable-length TX arrays on the 20 KiB STM32L072.
union FirmwareBuffers {
  struct { char line[LINE_SIZE]; uint8_t tx[LINE_SIZE], output[OUTPUT_SIZE]; } terminal;
  TestCapture capture;
};
extern FirmwareBuffers firmwareBuffers;
