// USB-C line terminal -> RF module UART. RF replies are forwarded unchanged.
#include <Arduino.h>
#include <usbd_cdc_if.h>
#include "TerminalCore.h"
#include "RfDmaRx.h"

#if !defined(USBCON) || !defined(USBD_USE_CDC)
#error "Enable USB CDC support, or select build-flash menu 14."
#endif

Uart SerialMioty(PC_11, PC_10); // RX, TX
constexpr uint32_t RF_BAUD = 921600, USB_BAUD = 115200;
constexpr size_t LINE_SIZE = 1024, OUTPUT_SIZE = 4096;
static_assert(SERIAL_RX_BUFFER_SIZE >= 1024, "Keep build_opt.h with this sketch");
char line[LINE_SIZE];
uint8_t tx[LINE_SIZE], output[OUTPUT_SIZE];
size_t lineSize = 0, txSize = 0, txOffset = 0;
size_t outputHead = 0, outputTail = 0;
bool discardLine = false, previousCR = false, wasOpen = false;

void serviceRf() {
  if (!SerialMioty) return;
  for (unsigned i = 0; i < 256 && SerialMioty.available(); ++i) {
    const size_t next = (outputHead + 1) % OUTPUT_SIZE;
    if (next == outputTail) break;
    const int c = SerialMioty.read();
    if (c >= 0) { output[outputHead] = static_cast<uint8_t>(c); outputHead = next; }
  }
  // Queue TX in small batches so RX continues during long commands.
  const int room = SerialMioty.availableForWrite();
  size_t n = txSize - txOffset;
  if (room <= 0 || !n) return;
  if (n > static_cast<size_t>(room)) n = room;
  if (n > 32) n = 32;
  txOffset += SerialMioty.write(tx + txOffset, n);
}

void serviceOutput() {
  if (!CDC_connected()) return;
  const int room = SerialUSB.availableForWrite();
  if (room <= 0) return;
  size_t n = outputHead >= outputTail ? outputHead - outputTail : OUTPUT_SIZE - outputTail;
  if (n > static_cast<size_t>(room)) n = room;
  if (n > 256) n = 256;
  if (n) outputTail = (outputTail + SerialUSB.write(output + outputTail, n)) % OUTPUT_SIZE;
}

void finishLine() {
  if (!discardLine && lineSize && SerialMioty && txOffset == txSize) {
    txSize = AtTerminal::decode(line, lineSize, tx, sizeof(tx));
    txOffset = 0;
  }
  lineSize = 0; discardLine = false;
}

void serviceInput() {
  // Leave subsequent lines in USB RX until the previous TX buffer is queued.
  for (unsigned i = 0; i < 32 && txOffset == txSize && SerialUSB.available(); ++i) {
    const int value = SerialUSB.read();
    if (value < 0) break;
    const char c = static_cast<char>(value);
    if (c == 3) {
      lineSize = 0; discardLine = previousCR = false;
      continue;
    }
    if (c == '\n' && previousCR) { previousCR = false; continue; }
    previousCR = c == '\r';
    if (c == '\r' || c == '\n') { finishLine(); continue; }
    if (discardLine) continue;
    if (c == '\b' || c == 127) { if (lineSize) --lineSize; continue; }
    if (lineSize < sizeof(line) - 1) line[lineSize++] = c;
    else discardLine = true; // Never send a truncated command.
  }
}

void setup() {
  __HAL_RCC_CRS_CLK_ENABLE();
  RCC_CRSInitTypeDef crs = {};
  crs.Prescaler = RCC_CRS_SYNC_DIV1; crs.Source = RCC_CRS_SYNC_SOURCE_USB;
  crs.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
  crs.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 1000);
  crs.ErrorLimitValue = RCC_CRS_ERRORLIMIT_DEFAULT;
  crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
  HAL_RCCEx_CRSConfig(&crs);
  SerialUSB.dtr(false);
  SerialMioty.begin(RF_BAUD, SERIAL_8N1);
  beginRfDmaRx();
  SerialUSB.begin(USB_BAUD);
}

void loop() {
  pollRfDmaRx();
  serviceRf(); // Also drain RF while USB is disconnected.
  const bool open = CDC_connected();
  if (open != wasOpen) {
    lineSize = 0; discardLine = previousCR = false;
    wasOpen = open;
  }
  if (open) serviceInput();
  serviceOutput();
}
