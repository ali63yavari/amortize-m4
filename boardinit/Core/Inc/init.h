#pragma once

#include "stm32f4xx_hal.h"

int SystemClock_Config(void);
void MX_GPIO_Init(void);
int MX_USART2_UART_Init(void);
void SWO_GPIO_Init(void);
