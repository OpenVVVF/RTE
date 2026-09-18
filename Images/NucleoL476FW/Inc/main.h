/**
  ******************************************************************************
  * @file    main.h
  * @brief   Header for main.c - Nucleo-L476RG RTE base firmware.
  ******************************************************************************
  */
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32l4xx_hal.h"

void Error_Handler(void);

/* On-board LED (LD2) */
#define LD2_Pin GPIO_PIN_5
#define LD2_GPIO_Port GPIOA

/* TIM1 three-phase PWM pin map (all AF1 on STM32L476):
 *   CH1  = PA8  (Arduino D7)
 *   CH1N = PA7  (Arduino D11)
 *   CH2  = PA9  (Arduino D8)
 *   CH2N = PB0  (Arduino A3)
 *   CH3  = PA10 (Arduino D2)
 *   CH3N = PB1  (morpho CN10-24)
 *   BKIN = PA6  (Arduino D12)
 *
 * SPI2 (PB12 CS, PB13 SCK, PB14 MISO, PB15 MOSI) is intentionally left
 * unconfigured / free for a future FPGA join link.
 */
#define PWM_CH1_Pin        GPIO_PIN_8
#define PWM_CH1_GPIO_Port  GPIOA
#define PWM_CH1N_Pin       GPIO_PIN_7
#define PWM_CH1N_GPIO_Port GPIOA
#define PWM_CH2_Pin        GPIO_PIN_9
#define PWM_CH2_GPIO_Port  GPIOA
#define PWM_CH2N_Pin       GPIO_PIN_0
#define PWM_CH2N_GPIO_Port GPIOB
#define PWM_CH3_Pin        GPIO_PIN_10
#define PWM_CH3_GPIO_Port  GPIOA
#define PWM_CH3N_Pin       GPIO_PIN_1
#define PWM_CH3N_GPIO_Port GPIOB
#define PWM_BKIN_Pin       GPIO_PIN_6
#define PWM_BKIN_GPIO_Port GPIOA

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
