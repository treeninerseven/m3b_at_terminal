#include <Arduino.h>

#if !defined(ARDUINO_GENERIC_L072RBTX)
#error "This clock configuration targets the M3Board STM32L072RBT6."
#endif

static_assert(HSI_VALUE == 16000000U, "This configuration requires HSI16");

// Override Core 3.0.0's weak SystemClock_Config before setup() runs.
// Use internal HSI16 for the 32 MHz system and UART bus clocks;
// USB uses the independent HSI48 oscillator.
extern "C" void SystemClock_Config(void)
{
  RCC_OscInitTypeDef oscillator = {};
  RCC_ClkInitTypeDef clocks = {};
  RCC_PeriphCLKInitTypeDef peripherals = {};

  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_HSE |
                              RCC_OSCILLATORTYPE_HSI48;
  oscillator.HSEState = RCC_HSE_OFF;
  oscillator.HSIState = RCC_HSI_ON;
  oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  oscillator.HSI48State = RCC_HSI48_ON;
  oscillator.PLL.PLLState = RCC_PLL_ON;
  oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  oscillator.PLL.PLLMUL = RCC_PLLMUL_4;
  oscillator.PLL.PLLDIV = RCC_PLLDIV_2; // 16 MHz * 4 / 2 = 32 MHz.
  if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) {
    Error_Handler();
  }

  clocks.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                     RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clocks.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clocks.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clocks.APB1CLKDivider = RCC_HCLK_DIV1;
  clocks.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_1) != HAL_OK) {
    Error_Handler();
  }

  // Keep LPUART1 on the PLL-derived 32 MHz bus, not directly on 16 MHz HSI.
  // Existing setup() still configures USB SOF calibration through CRS.
  peripherals.PeriphClockSelection = RCC_PERIPHCLK_LPUART1 | RCC_PERIPHCLK_USB;
  peripherals.Lpuart1ClockSelection = RCC_LPUART1CLKSOURCE_PCLK1;
  peripherals.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
  if (HAL_RCCEx_PeriphCLKConfig(&peripherals) != HAL_OK) {
    Error_Handler();
  }
}
