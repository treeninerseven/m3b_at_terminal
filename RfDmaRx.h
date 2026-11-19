#pragma once
#include <stdint.h>

void beginRfDmaRx();
void pollRfDmaRx();
struct RfRxStats {
  uint32_t uartErrors, overwrittenBytes, dmaErrors;
};
RfRxStats rfRxStats();
