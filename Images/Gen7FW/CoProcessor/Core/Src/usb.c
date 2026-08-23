/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usb.c
  * @brief   This file provides code for the configuration
  *          of the USB instances.
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
#include "usb.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

PCD_HandleTypeDef hpcd_USB_FS;
USBD_HandleTypeDef hUsbDeviceFS;

/* USB init function */

void MX_USB_DEVICE_Init(void)
{
  /* USER CODE BEGIN USB_Init 0 */

  /* USER CODE END USB_Init 0 */

  /* USER CODE BEGIN USB_Init 1 */

  /* USER CODE END USB_Init 1 */

  /* Init Device Library, add supported class and start the library */
  if (USBD_Init(&hUsbDeviceFS, &OpenVVVF_Desc, DEVICE_FS) != USBD_OK)
  {
    Error_Handler();
  }

  if (USBD_RegisterClass(&hUsbDeviceFS, USBD_CDC_CLASS) != USBD_OK)
  {
    Error_Handler();
  }

  if (USBD_CDC_RegisterInterface(&hUsbDeviceFS, &USBD_CDC_Interface_fops_FS) != USBD_OK)
  {
    Error_Handler();
  }

  if (USBD_Start(&hUsbDeviceFS) != USBD_OK)
  {
    Error_Handler();
  }

  /* USER CODE BEGIN USB_Init 2 */

  /* USER CODE END USB_Init 2 */
}

void HAL_PCD_MspInit(PCD_HandleTypeDef *pcdHandle)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
  if (pcdHandle->Instance == USB)
  {
    /* USER CODE BEGIN USB_MspInit 0 */

    /* USER CODE END USB_MspInit 0 */

    /** Initializes the peripherals clocks
    */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USB;
    PeriphClkInit.UsbClockSelection = RCC_USBCLKSOURCE_PLL;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      Error_Handler();
    }

    /* USB clock enable */
    __HAL_RCC_USB_CLK_ENABLE();

    /* USB DM/DP GPIO configuration */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* USB interrupt Init */
    HAL_NVIC_SetPriority(USB_LP_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USB_LP_IRQn);
    /* USER CODE BEGIN USB_MspInit 1 */

    /* USER CODE END USB_MspInit 1 */
  }
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef *pcdHandle)
{
  if (pcdHandle->Instance == USB)
  {
    /* USER CODE BEGIN USB_MspDeInit 0 */

    /* USER CODE END USB_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USB_CLK_DISABLE();

    /* USB interrupt DeInit */
    HAL_NVIC_DisableIRQ(USB_LP_IRQn);
    /* USER CODE BEGIN USB_MspDeInit 1 */

    /* USER CODE END USB_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
