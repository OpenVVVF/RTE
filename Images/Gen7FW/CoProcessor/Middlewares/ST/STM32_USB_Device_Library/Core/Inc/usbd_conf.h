/**
  ******************************************************************************
  * @file    usbd_conf.h
  * @author  MCD Application Team
  * @brief   Header file for the USB Device configuration file
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
#ifndef __USBD_CONF_H
#define __USBD_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32g4xx.h"
#include "stm32g4xx_hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @addtogroup STM32_USB_DEVICE_LIBRARY
  * @{
  */

/** @defgroup USBD_CONF
  * @brief USB device low level driver configuration file
  * @{
  */

/** @defgroup USBD_CONF_Exported_Defines
  * @{
  */

#define USBD_MAX_NUM_INTERFACES                     1U
#define USBD_MAX_NUM_CONFIGURATION                  1U
#define USBD_MAX_STR_DESC_SIZ                       0x100U
#define USBD_SELF_POWERED                           1U
#define USBD_DEBUG_LEVEL                            0U

/* CDC Class Config */
#define USBD_CDC_INTERVAL                           2000U

/* USB Device speed identifier */
#define DEVICE_FS                                   1U

/* Memory management macros */
#define USBD_malloc         (void *)USBD_static_malloc
#define USBD_free           USBD_static_free
#define USBD_memset         memset
#define USBD_memcpy         memcpy
#define USBD_Delay          HAL_Delay

/* DEBUG macros */
#if (USBD_DEBUG_LEVEL > 0U)
#define  USBD_UsrLog(...)   do { \
                                 printf(__VA_ARGS__); \
                                 printf("\n"); \
                               } while (0)
#else
#define USBD_UsrLog(...) do {} while (0)
#endif /* (USBD_DEBUG_LEVEL > 0U) */

#if (USBD_DEBUG_LEVEL > 1U)
#define  USBD_ErrLog(...) do { \
                               printf("ERROR: "); \
                               printf(__VA_ARGS__); \
                               printf("\n"); \
                             } while (0)
#else
#define USBD_ErrLog(...) do {} while (0)
#endif /* (USBD_DEBUG_LEVEL > 1U) */

#if (USBD_DEBUG_LEVEL > 2U)
#define  USBD_DbgLog(...)   do { \
                                 printf("DEBUG: "); \
                                 printf(__VA_ARGS__); \
                                 printf("\n"); \
                               } while (0)
#else
#define USBD_DbgLog(...) do {} while (0)
#endif /* (USBD_DEBUG_LEVEL > 2U) */

/**
  * @}
  */

/** @defgroup USBD_CONF_Exported_Types
  * @{
  */
/**
  * @}
  */

/** @defgroup USBD_CONF_Exported_Macros
  * @{
  */
/**
  * @}
  */

/** @defgroup USBD_CONF_Exported_Variables
  * @{
  */
/**
  * @}
  */

/** @defgroup USBD_CONF_Exported_FunctionsPrototype
  * @{
  */
void *USBD_static_malloc(uint32_t size);
void USBD_static_free(void *p);

/**
  * @}
  */

#ifdef __cplusplus
}
#endif

#endif /* __USBD_CONF_H */

/**
  * @}
  */

/**
  * @}
  */
