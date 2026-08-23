/**
  ******************************************************************************
  * @file    usbd_desc.h
  * @author  MCD Application Team
  * @brief   Header for the USB device descriptors file
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
#ifndef __USBD_DESC_H
#define __USBD_DESC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "usbd_def.h"

/** @addtogroup STM32_USB_DEVICE_LIBRARY
  * @{
  */

/** @defgroup USBD_DESC
  * @brief USB device descriptors module
  * @{
  */

/** @defgroup USBD_DESC_Exported_Constants
  * @{
  */
#define USBD_VID                        0x0483U
#define USBD_PID                        0x5742U
#define USBD_LANGID_STRING              0x0409U
#define USBD_MANUFACTURER_STRING        "OpenVVVF"
#define USBD_PRODUCT_FS_STRING          "OpenVVVF Virtual COM Port"
#define USBD_CONFIGURATION_FS_STRING    "CDC Config"
#define USBD_INTERFACE_FS_STRING        "CDC Interface"

/**
  * @}
  */

/** @defgroup USBD_DESC_Exported_Defines
  * @{
  */
/**
  * @}
  */

/** @defgroup USBD_DESC_Exported_TypesDefinitions
  * @{
  */
/**
  * @}
  */

/** @defgroup USBD_DESC_Exported_Macros
  * @{
  */
/**
  * @}
  */

/** @defgroup USBD_DESC_Exported_Variables
  * @{
  */
extern USBD_DescriptorsTypeDef OpenVVVF_Desc;
/**
  * @}
  */

/** @defgroup USBD_DESC_Exported_FunctionsPrototype
  * @{
  */
/**
  * @}
  */

#ifdef __cplusplus
}
#endif

#endif /* __USBD_DESC_H */

/**
  * @}
  */

/**
  * @}
  */
