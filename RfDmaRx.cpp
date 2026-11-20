#include <Arduino.h>
#include "RfDmaRx.h"

extern Uart SerialMioty;
extern "C" serial_t *get_serial_obj(UART_HandleTypeDef *uart);

namespace {
constexpr uint32_t DMA_SIZE = 2048;
alignas(4) volatile uint8_t dmaBuffer[DMA_SIZE];
volatile uint32_t dmaWraps = 0;
volatile bool dmaBroken = false;
uint32_t consumed = 0;
serial_t *receiver = nullptr;

} // namespace

void beginRfDmaRx()
{
  UART_HandleTypeDef *uart = SerialMioty.getHandle();
  // Abort RX only: leave the Arduino TX state and interrupt handlers intact.
  if (HAL_UART_AbortReceive(uart) != HAL_OK) Error_Handler();
  uart->RxISR = nullptr;
  receiver = get_serial_obj(uart);
  __HAL_RCC_DMA1_CLK_ENABLE();
  HAL_NVIC_DisableIRQ(DMA1_Channel2_3_IRQn);
  CLEAR_BIT(DMA1_Channel3->CCR, DMA_CCR_EN);
  DMA1->IFCR = DMA_IFCR_CGIF3;
  dmaWraps = 0; consumed = 0; dmaBroken = false;

  // RM0376 table 51: LPUART1_RX is channel 3, request 5 (C3S = 0101).
  MODIFY_REG(DMA1_CSELR->CSELR, DMA_CSELR_C3S, 5U << DMA_CSELR_C3S_Pos);
  DMA1_Channel3->CPAR = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&LPUART1->RDR));
  DMA1_Channel3->CMAR = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dmaBuffer));
  DMA1_Channel3->CNDTR = DMA_SIZE;
  // Peripheral -> memory, byte widths, fixed peripheral, incrementing memory.
  DMA1_Channel3->CCR = DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_PL_1 |
                       DMA_CCR_TCIE | DMA_CCR_TEIE;
  HAL_NVIC_ClearPendingIRQ(DMA1_Channel2_3_IRQn);
  HAL_NVIC_SetPriority(DMA1_Channel2_3_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel2_3_IRQn);
  SET_BIT(DMA1_Channel3->CCR, DMA_CCR_EN);
  // The core's 8N1 setup leaves DDRE and OVRDIS cleared. Keep error detection
  // enabled; poll its flags instead of running a UART interrupt for each byte.
  SET_BIT(uart->Instance->CR3, USART_CR3_DMAR);
}

namespace {
uint32_t producedBytes()
{
  // Account for a completed DMA lap even if its IRQ has not run yet. Reading
  // CNDTR again after TCIF avoids combining a pre-wrap position with a new lap.
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  uint32_t wraps = dmaWraps;
  uint32_t remaining = DMA1_Channel3->CNDTR;
  if (DMA1->ISR & DMA_ISR_TCIF3) {
    ++wraps;
    remaining = DMA1_Channel3->CNDTR;
  }
  __set_PRIMASK(primask);
  // Zero can be observed at reload. Treat it as position zero; drainDma()
  // defers a stale pre-wrap snapshot rather than inventing an extra lap.
  const uint32_t position = remaining ? DMA_SIZE - remaining : 0;
  return wraps * DMA_SIZE + position;
}

void drainDma(uint32_t produced)
{
  uint32_t available = produced - consumed;
  if (available & 0x80000000U) return; // Transient stale reload snapshot.
  if (available >= DMA_SIZE) {
    // The oldest bytes may already be overwritten. Discard this entire unread
    // interval rather than returning a possibly torn response.
    consumed = produced;
    return;
  }
  for (unsigned budget = 0; available && budget < 256; ++budget) {
    const uint16_t head = receiver->rx_head;
    const uint16_t next = (head + 1U) % SERIAL_RX_BUFFER_SIZE;
    if (next == receiver->rx_tail) break; // Defer until loop() drains the ring.
    receiver->rx_buff[head] = dmaBuffer[consumed % DMA_SIZE];
    receiver->rx_head = next;
    ++consumed; --available;
  }
}

} // namespace

void pollRfDmaRx()
{
  if (!receiver || dmaBroken) return;
  UART_HandleTypeDef *uart = SerialMioty.getHandle();
  // ICR error-clear bits match ISR positions. Clear ORE so RX can continue.
  uart->Instance->ICR = uart->Instance->ISR &
      (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE);
  drainDma(producedBytes());
}

extern "C" void DMA1_Channel2_3_IRQHandler(void)
{
  // This sketch reserves channel 3 and its shared IRQ; channel 2 is unused.
  // Only one IRQ per DMA lap, plus exceptional DMA transfer errors.
  const uint32_t flags = DMA1->ISR;
  if (flags & (DMA_ISR_TCIF3 | DMA_ISR_TEIF3)) {
    DMA1->IFCR = DMA_IFCR_CGIF3;
    if (flags & DMA_ISR_TCIF3) ++dmaWraps;
    if (flags & DMA_ISR_TEIF3) {
      dmaBroken = true;
      CLEAR_BIT(DMA1_Channel3->CCR, DMA_CCR_EN);
    }
  }
}
