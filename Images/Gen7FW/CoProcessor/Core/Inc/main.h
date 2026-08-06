/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32g4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define GATE_DRIVE_PWR1_FEEDBACK_Pin GPIO_PIN_14
#define GATE_DRIVE_PWR1_FEEDBACK_GPIO_Port GPIOC
#define GATE_DRIVE_PWR2_FEEDBACK_Pin GPIO_PIN_15
#define GATE_DRIVE_PWR2_FEEDBACK_GPIO_Port GPIOC
#define CURSENSE_DC_LINK_REF_Pin GPIO_PIN_0
#define CURSENSE_DC_LINK_REF_GPIO_Port GPIOC
#define THROTTLE_A_Pin GPIO_PIN_1
#define THROTTLE_A_GPIO_Port GPIOC
#define THROTTLE_B_Pin GPIO_PIN_2
#define THROTTLE_B_GPIO_Port GPIOC
#define IGBT_TEMP_Pin GPIO_PIN_3
#define IGBT_TEMP_GPIO_Port GPIOC
#define CURSENSE_PH_U_SIG_Pin GPIO_PIN_0
#define CURSENSE_PH_U_SIG_GPIO_Port GPIOA
#define CURSENSE_PH_U_REF_Pin GPIO_PIN_1
#define CURSENSE_PH_U_REF_GPIO_Port GPIOA
#define CURSENSE_PH_V_SIG_Pin GPIO_PIN_2
#define CURSENSE_PH_V_SIG_GPIO_Port GPIOA
#define CURSENSE_PH_V_REF_Pin GPIO_PIN_3
#define CURSENSE_PH_V_REF_GPIO_Port GPIOA
#define HALL_W_Pin GPIO_PIN_4
#define HALL_W_GPIO_Port GPIOA
#define ENCODER_SIN_HALL_U_Pin GPIO_PIN_5
#define ENCODER_SIN_HALL_U_GPIO_Port GPIOA
#define CURSENSE_PH_W_SIG_Pin GPIO_PIN_6
#define CURSENSE_PH_W_SIG_GPIO_Port GPIOA
#define CURSENSE_PH_W_REF_Pin GPIO_PIN_7
#define CURSENSE_PH_W_REF_GPIO_Port GPIOA
#define CURSENSE_DC_LINK_SIG_Pin GPIO_PIN_4
#define CURSENSE_DC_LINK_SIG_GPIO_Port GPIOC
#define MOTOR_TEMP_Pin GPIO_PIN_5
#define MOTOR_TEMP_GPIO_Port GPIOC
#define DC_LINK_CURRENT_COMPARITOR_FB_Pin GPIO_PIN_0
#define DC_LINK_CURRENT_COMPARITOR_FB_GPIO_Port GPIOB
#define PHASE_CURRENT_COMPARATOR_FB_Pin GPIO_PIN_1
#define PHASE_CURRENT_COMPARATOR_FB_GPIO_Port GPIOB
#define TIMER_SYNC_FROM_MASTER_Pin GPIO_PIN_2
#define TIMER_SYNC_FROM_MASTER_GPIO_Port GPIOB
#define GATE_DRIVER_FAULT_IN_Pin GPIO_PIN_10
#define GATE_DRIVER_FAULT_IN_GPIO_Port GPIOB
#define ENCODER_COS_HALL_V_Pin GPIO_PIN_11
#define ENCODER_COS_HALL_V_GPIO_Port GPIOB
#define PH_V_LOW_Pin GPIO_PIN_12
#define PH_V_LOW_GPIO_Port GPIOB
#define PH_V_HIGH_Pin GPIO_PIN_13
#define PH_V_HIGH_GPIO_Port GPIOB
#define PH_W_LOW_Pin GPIO_PIN_14
#define PH_W_LOW_GPIO_Port GPIOB
#define PH_W_HIGH_Pin GPIO_PIN_15
#define PH_W_HIGH_GPIO_Port GPIOB
#define GATE_DRIVE_READY_Pin GPIO_PIN_6
#define GATE_DRIVE_READY_GPIO_Port GPIOC
#define GATE_DRIVE_PWR_ENABLE_Pin GPIO_PIN_7
#define GATE_DRIVE_PWR_ENABLE_GPIO_Port GPIOC
#define BOOTSEL_MAIN_MCU_Pin GPIO_PIN_8
#define BOOTSEL_MAIN_MCU_GPIO_Port GPIOC
#define RESET_MAIN_MCU_Pin GPIO_PIN_9
#define RESET_MAIN_MCU_GPIO_Port GPIOC
#define PH_U_LOW_Pin GPIO_PIN_8
#define PH_U_LOW_GPIO_Port GPIOA
#define PH_U_HIGH_Pin GPIO_PIN_9
#define PH_U_HIGH_GPIO_Port GPIOA
#define GATE_DRIVER_RESET_Pin GPIO_PIN_10
#define GATE_DRIVER_RESET_GPIO_Port GPIOA
#define USER_DIN_1_Pin GPIO_PIN_15
#define USER_DIN_1_GPIO_Port GPIOA
#define USER_DIN_2_Pin GPIO_PIN_12
#define USER_DIN_2_GPIO_Port GPIOC
#define USER_DIN_3_Pin GPIO_PIN_2
#define USER_DIN_3_GPIO_Port GPIOD
#define USER_DIN_4_Pin GPIO_PIN_7
#define USER_DIN_4_GPIO_Port GPIOB
#define INTERMCU_SYNC_LINE_Pin GPIO_PIN_9
#define INTERMCU_SYNC_LINE_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
