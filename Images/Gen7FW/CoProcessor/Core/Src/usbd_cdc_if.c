/**
  ******************************************************************************
  * @file    usbd_cdc_if.c
  * @author  MCD Application Team
  * @brief   USB Device CDC interface file
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

/* Includes ------------------------------------------------------------------*/
#include "usbd_cdc_if.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Create buffer for reception and transmission           */
/* It's up to user to redefine and/or remove those define */
/** Received data over USB are stored in this buffer      */
uint8_t UserRxBufferFS[APP_RX_DATA_SIZE];

/** Data to send over USB CDC are stored in this buffer   */
uint8_t UserTxBufferFS[APP_TX_DATA_SIZE];

/* USB handler declaration */
extern USBD_HandleTypeDef hUsbDeviceFS;

/* Private function prototypes -----------------------------------------------*/
static int8_t CDC_Init_FS(void);
static int8_t CDC_DeInit_FS(void);
static int8_t CDC_Control_FS(uint8_t cmd, uint8_t *pbuf, uint16_t length);
static int8_t CDC_Receive_FS(uint8_t *pbuf, uint32_t *Len);
static int8_t CDC_TransmitCplt_FS(uint8_t *pbuf, uint32_t *Len, uint8_t epnum);

USBD_CDC_ItfTypeDef USBD_CDC_Interface_fops_FS =
{
  CDC_Init_FS,
  CDC_DeInit_FS,
  CDC_Control_FS,
  CDC_Receive_FS,
  CDC_TransmitCplt_FS
};

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Initializes the CDC media low layer over the FS USB IP
  * @retval USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_Init_FS(void)
{
  /* Set Application Buffers */
  (void)USBD_CDC_SetTxBuffer(&hUsbDeviceFS, UserTxBufferFS, 0);
  (void)USBD_CDC_SetRxBuffer(&hUsbDeviceFS, UserRxBufferFS);
  (void)USBD_CDC_ReceivePacket(&hUsbDeviceFS);
  return (USBD_OK);
}

/**
  * @brief  DeInitializes the CDC media low layer
  * @retval USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_DeInit_FS(void)
{
  return (USBD_OK);
}

/**
  * @brief  Manage the CDC class requests
  * @param  cmd: Command code
  * @param  pbuf: Buffer containing command data (request parameters)
  * @param  length: Number of data to be sent (in bytes)
  * @retval USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_Control_FS(uint8_t cmd, uint8_t *pbuf, uint16_t length)
{
  UNUSED(length);
  USBD_CDC_LineCodingTypeDef *plinecoding = (USBD_CDC_LineCodingTypeDef *)pbuf;

  switch (cmd)
  {
    case CDC_SEND_ENCAPSULATED_COMMAND:
      break;

    case CDC_GET_ENCAPSULATED_RESPONSE:
      break;

    case CDC_SET_COMM_FEATURE:
      break;

    case CDC_GET_COMM_FEATURE:
      break;

    case CDC_CLEAR_COMM_FEATURE:
      break;

    /*******************************************************************************
                      Line Coding Structure
    -------------------------------------------------------------------------------
    dwDTERate|                |Data Bits|            Stop Bits
    -----------|---------------|---------|-------------------------------
    4 bytes    | 1 byte        |  1 byte |  1 byte
    ************************************************************************************/

    case CDC_SET_LINE_CODING:
      break;

    case CDC_GET_LINE_CODING:
      plinecoding->bitrate = 115200;
      plinecoding->format = 0;
      plinecoding->paritytype = 0;
      plinecoding->datatype = 8;
      break;

    case CDC_SET_CONTROL_LINE_STATE:
      break;

    case CDC_SEND_BREAK:
      break;

    default:
      break;
  }

  return (USBD_OK);
}

/**
  * @brief  Data received over USB OUT endpoint are sent over CDC interface
  *         through this function.
  *
  *         @note
  *         This function will issue a NAK packet on any OUT packet received on
  *         USB endpoint until exiting this function. If you also exit with keyboard
  *         interface you could call this function in a loop on a dedicated task.
  *
  * @param  Buf: Buffer of data to be received
  * @param  Len: Number of data received (in bytes)
  * @retval USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_Receive_FS(uint8_t *Buf, uint32_t *Len)
{
  if (*Len > APP_TX_DATA_SIZE)
  {
    *Len = APP_TX_DATA_SIZE;
  }

  /* Simple echo: copy to tx buffer and transmit back what was received */
  (void)memcpy(UserTxBufferFS, Buf, *Len);
  (void)USBD_CDC_SetTxBuffer(&hUsbDeviceFS, UserTxBufferFS, (uint16_t)*Len);
  (void)USBD_CDC_TransmitPacket(&hUsbDeviceFS);

  /* Set a reception buffer to receive next packet */
  (void)USBD_CDC_ReceivePacket(&hUsbDeviceFS);
  return (USBD_OK);
}

/**
  * @brief  CDC_Transmit_FS
  *         Data to send over USB IN endpoint are sent over CDC interface
  *         through this function.
  *         @note
  *
  *
  * @param  Buf: Buffer of data to be sent
  * @param  Len: Number of data to be sent (in bytes)
  * @retval USBD_OK if all operations are OK else USBD_FAIL or USBD_BUSY
  */
uint8_t CDC_Transmit_FS(uint8_t *Buf, uint16_t Len)
{
  uint8_t result = USBD_OK;

  (void)USBD_CDC_SetTxBuffer(&hUsbDeviceFS, Buf, Len);
  result = USBD_CDC_TransmitPacket(&hUsbDeviceFS);

  return result;
}

/**
  * @brief  CDC_TransmitCplt_FS
  *         Data transmitted callback
  *
  *         @note
  *         This function is IN transfer complete callback used to inform user that
  *         the submitted Data is being sent to the Host.
  *
  * @param  Buf: Buffer of data to be received
  * @param  Len: Number of data received (in bytes)
  * @retval USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_TransmitCplt_FS(uint8_t *Buf, uint32_t *Len, uint8_t epnum)
{
  UNUSED(Buf);
  UNUSED(Len);
  UNUSED(epnum);

  return (USBD_OK);
}
