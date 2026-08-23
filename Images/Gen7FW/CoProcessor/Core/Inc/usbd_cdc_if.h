/**
  ******************************************************************************
  * @file    usbd_cdc_if.h
  * @author  MCD Application Team
  * @brief   Header for usbd_cdc_if.c file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2015 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __USBD_CDC_IF_H
#define __USBD_CDC_IF_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "usbd_cdc.h"

/** @addtogroup STM32_USB_DEVICE_LIBRARY
  * @{
  */

/** @defgroup USBD_CDC_IF
  * @brief header file for usbd_cdc_if.c
  * @{
  */

/** @defgroup USBD_CDC_IF_Exported_Defines
  * @{
  */
/* Define size for the receive and transmit buffer over CDC */
#define APP_RX_DATA_SIZE  64
#define APP_TX_DATA_SIZE  64
/**
  * @}
  */

/** @defgroup USBD_CDC_IF_Exported_Types
  * @{
  */
/* USER CODE BEGIN EXPORTED_TYPES */
/* USER CODE END EXPORTED_TYPES */

/**
  * @}
  */

/** @defgroup USBD_CDC_IF_Exported_Macros
  * @{
  */
/* USER CODE BEGIN EXPORTED_MACRO */
/* USER CODE END EXPORTED_MACRO */

/**
  * @}
  */

/** @defgroup USBD_CDC_IF_Exported_Variables
  * @{
  */
extern USBD_CDC_ItfTypeDef USBD_CDC_Interface_fops_FS;

/**
  * @}
  */

/** @defgroup USBD_CDC_IF_Exported_FunctionsPrototype
  * @{
  */
uint8_t CDC_Transmit_FS(uint8_t *Buf, uint16_t Len);

/**
  * @}
  */

#ifdef __cplusplus
}
#endif

#endif /* __USBD_CDC_IF_H */

/**
  * @}
  */

/**
  * @}
  */
