/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "init.h"
#include "stm32f4xx_hal_uart.h"
#include <stdio.h>
#include <string.h>

UART_HandleTypeDef huart2;

// void bench_step2_run(void);
// void bench_step2_stack_run(void);
void bench_step2_audit_run(void);
void print_clock();
void rng_direct_stress_test();
void rng_direct_stress_test_v2();


int test(void) {

  HAL_Init();

  if (SystemClock_Config() != 0) {
    Error_Handler();
  }

  MX_GPIO_Init();
  if (MX_USART2_UART_Init() != 0) {
    Error_Handler();
  }

  const char *startup_msg = "\r\nUSART2 serial test started on STM32F407\r\n";
  HAL_UART_Transmit(&huart2, (uint8_t *)startup_msg, strlen(startup_msg),
                    HAL_MAX_DELAY);

  while (1) {
    /* USER CODE END WHILE */
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_12 | GPIO_PIN_13, GPIO_PIN_SET);
    HAL_Delay(1000);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_12 | GPIO_PIN_13, GPIO_PIN_RESET);

    ITM_SendChar('A');
    ITM_SendChar('\r');
    ITM_SendChar('\n');

    HAL_UART_Transmit(&huart2, (uint8_t *)startup_msg, strlen(startup_msg),
                      HAL_MAX_DELAY);

    HAL_Delay(1000);
  }
}

int main(void) {
  HAL_Init();

  SystemClock_Config();

  MX_GPIO_Init();
  MX_USART2_UART_Init();

  /*
   * If CubeMX generated MX_RNG_Init(), you may call it here too,
   * but the benchmark code already enables RNG directly.
   */
  /* MX_RNG_Init(); */

  // bench_step2_stack_run();

  print_clock();
  rng_direct_stress_test_v2();

  // bench_step2_audit_run();

  while (1) {
  }
}

void Error_Handler(void) {
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1) {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
 * @brief  Reports the name of the source file and the source line number
 *         where the assert_param error has occurred.
 * @param  file: pointer to the source file name
 * @param  line: assert_param error line source number
 * @retval None
 */
void assert_failed(uint8_t *file, uint32_t line) {
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line
     number, ex: printf("Wrong parameters value: file %s on line %d\r\n", file,
     line) */
  /* USER CODE END 6 */
}
#endif
