/**
  ******************************************************************************
  * @file    stm32l4xx_it.c
  * @brief   Interrupt service routines.
  ******************************************************************************
  */

#include "main.h"
#include "stm32l4xx_it.h"
#include "tim.h"

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/

void NMI_Handler(void)
{
    while (1)
    {
    }
}

void HardFault_Handler(void)
{
    while (1)
    {
    }
}

void MemManage_Handler(void)
{
    while (1)
    {
    }
}

void BusFault_Handler(void)
{
    while (1)
    {
    }
}

void UsageFault_Handler(void)
{
    while (1)
    {
    }
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

void SysTick_Handler(void)
{
    HAL_IncTick();
}

/******************************************************************************/
/* STM32L4xx Peripheral Interrupt Handlers                                    */
/******************************************************************************/

/**
  * @brief TIM1 update interrupt (shared with TIM16 on L4).
  *
  * Dispatches to HAL_TIM_PeriodElapsedCallback in pwm.cpp, which hosts the
  * RTE tim_isr timing domain.
  */
void TIM1_UP_TIM16_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim1);
}

/**
  * @brief TIM1 break interrupt (shared with TIM15 on L4).
  */
void TIM1_BRK_TIM15_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim1);
}
