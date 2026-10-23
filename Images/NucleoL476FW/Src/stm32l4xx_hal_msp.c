/**
  ******************************************************************************
  * @file    stm32l4xx_hal_msp.c
  * @brief   HAL MSP (MCU Support Package) init.
  *          Peripheral-specific MSP handlers live next to their peripherals
  *          (e.g. TIM1 in Src/tim.c).
  ******************************************************************************
  */

#include "main.h"

void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
}
