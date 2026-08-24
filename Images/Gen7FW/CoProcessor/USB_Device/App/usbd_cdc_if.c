/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : usbd_cdc_if.c
  * @version        : v3.0_Cube
  * @brief          : Usb device for Virtual Com Port.
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
#include "usbd_cdc_if.h"

/* USER CODE BEGIN INCLUDE */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "usart.h"
#include "main.h"
/* USER CODE END INCLUDE */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

/* USER CODE END PV */

/** @addtogroup STM32_USB_OTG_DEVICE_LIBRARY
  * @brief Usb device library.
  * @{
  */

/** @addtogroup USBD_CDC_IF
  * @{
  */

/** @defgroup USBD_CDC_IF_Private_TypesDefinitions USBD_CDC_IF_Private_TypesDefinitions
  * @brief Private types.
  * @{
  */

/* USER CODE BEGIN PRIVATE_TYPES */

/* USER CODE END PRIVATE_TYPES */

/**
  * @}
  */

/** @defgroup USBD_CDC_IF_Private_Defines USBD_CDC_IF_Private_Defines
  * @brief Private defines.
  * @{
  */

/* USER CODE BEGIN PRIVATE_DEFINES */
/* USER CODE END PRIVATE_DEFINES */

/**
  * @}
  */

/** @defgroup USBD_CDC_IF_Private_Macros USBD_CDC_IF_Private_Macros
  * @brief Private macros.
  * @{
  */

/* USER CODE BEGIN PRIVATE_MACRO */

/* USER CODE END PRIVATE_MACRO */

/**
  * @}
  */

/** @defgroup USBD_CDC_IF_Private_Variables USBD_CDC_IF_Private_Variables
  * @brief Private variables.
  * @{
  */
/* Create buffer for reception and transmission           */
/* It's up to user to redefine and/or remove those define */
/** Received data over USB are stored in this buffer      */
uint8_t UserRxBufferFS[APP_RX_DATA_SIZE];

/** Data to send over USB CDC are stored in this buffer   */
uint8_t UserTxBufferFS[APP_TX_DATA_SIZE];

/* USER CODE BEGIN PRIVATE_VARIABLES */
static volatile uint8_t bridge_mode = 0U;
static volatile uint8_t enter_bridge_request = 0U;
static volatile uint8_t bridge_validate = 1U;
static uint8_t bridge_cmd_idx = 0U;
static const char *bridge_cmd = "BOOTLOADER\r\n";
static uint8_t dumb_bridge_cmd_idx = 0U;
static const char *dumb_bridge_cmd = "BRIDGE\r\n";

static uint8_t uart_rx_buf[CDC_BRIDGE_BUF_SIZE];
static volatile uint16_t uart_rx_head = 0U;
static volatile uint16_t uart_rx_tail = 0U;
static uint8_t uart_rx_byte;

static uint8_t uart_tx_buf[CDC_BRIDGE_BUF_SIZE];
static volatile uint16_t uart_tx_head = 0U;
static volatile uint16_t uart_tx_tail = 0U;
static volatile uint8_t uart_tx_active = 0U;
static uint8_t uart_tx_active_byte;

static volatile uint8_t cdc_tx_busy = 0U;
/* USER CODE END PRIVATE_VARIABLES */

/**
  * @}
  */

/** @defgroup USBD_CDC_IF_Exported_Variables USBD_CDC_IF_Exported_Variables
  * @brief Public variables.
  * @{
  */

extern USBD_HandleTypeDef hUsbDeviceFS;

/* USER CODE BEGIN EXPORTED_VARIABLES */

/* USER CODE END EXPORTED_VARIABLES */

/**
  * @}
  */

/** @defgroup USBD_CDC_IF_Private_FunctionPrototypes USBD_CDC_IF_Private_FunctionPrototypes
  * @brief Private functions declaration.
  * @{
  */

static int8_t CDC_Init_FS(void);
static int8_t CDC_DeInit_FS(void);
static int8_t CDC_Control_FS(uint8_t cmd, uint8_t* pbuf, uint16_t length);
static int8_t CDC_Receive_FS(uint8_t* pbuf, uint32_t *Len);
static int8_t CDC_TransmitCplt_FS(uint8_t *pbuf, uint32_t *Len, uint8_t epnum);

/* USER CODE BEGIN PRIVATE_FUNCTIONS_DECLARATION */
static void Bridge_UartTxStart(void);
static void MainMCU_ResetToBootloader(void);
static void MainMCU_ResetToApp(void);
static uint8_t Bootloader_ValidateSync(void);
static void CDC_SendString(const char *str);
static void CDC_DebugPrintf(const char *fmt, ...);
static void CDC_DebugPinStates(const char *label);
/* USER CODE END PRIVATE_FUNCTIONS_DECLARATION */

/**
  * @}
  */

USBD_CDC_ItfTypeDef USBD_Interface_fops_FS =
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
  /* USER CODE BEGIN 3 */
  /* Set Application Buffers */
  USBD_CDC_SetTxBuffer(&hUsbDeviceFS, UserTxBufferFS, 0);
  USBD_CDC_SetRxBuffer(&hUsbDeviceFS, UserRxBufferFS);
  return (USBD_OK);
  /* USER CODE END 3 */
}

/**
  * @brief  DeInitializes the CDC media low layer
  * @retval USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_DeInit_FS(void)
{
  /* USER CODE BEGIN 4 */
  return (USBD_OK);
  /* USER CODE END 4 */
}

/**
  * @brief  Manage the CDC class requests
  * @param  cmd: Command code
  * @param  pbuf: Buffer containing command data (request parameters)
  * @param  length: Number of data to be sent (in bytes)
  * @retval Result of the operation: USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_Control_FS(uint8_t cmd, uint8_t* pbuf, uint16_t length)
{
  /* USER CODE BEGIN 5 */
  switch(cmd)
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

  /*******************************************************************************/
  /* Line Coding Structure                                                       */
  /*-----------------------------------------------------------------------------*/
  /* Offset | Field       | Size | Value  | Description                          */
  /* 0      | dwDTERate   |   4  | Number |Data terminal rate, in bits per second*/
  /* 4      | bCharFormat |   1  | Number | Stop bits                            */
  /*                                        0 - 1 Stop bit                       */
  /*                                        1 - 1.5 Stop bits                    */
  /*                                        2 - 2 Stop bits                      */
  /* 5      | bParityType |  1   | Number | Parity                               */
  /*                                        0 - None                             */
  /*                                        1 - Odd                              */
  /*                                        2 - Even                             */
  /*                                        3 - Mark                             */
  /*                                        4 - Space                            */
  /* 6      | bDataBits  |   1   | Number Data bits (5, 6, 7, 8 or 16).          */
  /*******************************************************************************/
    case CDC_SET_LINE_CODING:

    break;

    case CDC_GET_LINE_CODING:

    break;

    case CDC_SET_CONTROL_LINE_STATE:

    break;

    case CDC_SEND_BREAK:

    break;

  default:
    break;
  }

  return (USBD_OK);
  /* USER CODE END 5 */
}

/**
  * @brief  Data received over USB OUT endpoint are sent over CDC interface
  *         through this function.
  *
  *         @note
  *         This function will issue a NAK packet on any OUT packet received on
  *         USB endpoint until exiting this function. If you exit this function
  *         before transfer is complete on CDC interface (ie. using DMA controller)
  *         it will result in receiving more data while previous ones are still
  *         not sent.
  *
  * @param  Buf: Buffer of data to be received
  * @param  Len: Number of data received (in bytes)
  * @retval Result of the operation: USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_Receive_FS(uint8_t* Buf, uint32_t *Len)
{
  /* USER CODE BEGIN 6 */
  USBD_CDC_SetRxBuffer(&hUsbDeviceFS, &Buf[0]);
  USBD_CDC_ReceivePacket(&hUsbDeviceFS);

  if (bridge_mode != 0U)
  {
    for (uint32_t i = 0U; i < *Len; i++)
    {
      uint16_t next_head = (uart_tx_head + 1U) % CDC_BRIDGE_BUF_SIZE;
      if (next_head != uart_tx_tail)
      {
        uart_tx_buf[uart_tx_head] = Buf[i];
        uart_tx_head = next_head;
      }
    }
    Bridge_UartTxStart();
  }
  else
  {
    for (uint32_t i = 0U; i < *Len; i++)
    {
      /* BOOTLOADER command: reset, validate sync, then bridge */
      if (Buf[i] == (uint8_t)bridge_cmd[bridge_cmd_idx])
      {
        bridge_cmd_idx++;
        if (bridge_cmd[bridge_cmd_idx] == '\0')
        {
          bridge_validate = 1U;
          enter_bridge_request = 1U;
          bridge_cmd_idx = 0U;
        }
      }
      else
      {
        bridge_cmd_idx = 0U;
        if (Buf[i] == (uint8_t)bridge_cmd[0])
        {
          bridge_cmd_idx = 1U;
        }
      }

      /* BRIDGE command: reset and bridge, let the host tool do the sync */
      if (Buf[i] == (uint8_t)dumb_bridge_cmd[dumb_bridge_cmd_idx])
      {
        dumb_bridge_cmd_idx++;
        if (dumb_bridge_cmd[dumb_bridge_cmd_idx] == '\0')
        {
          bridge_validate = 0U;
          enter_bridge_request = 1U;
          dumb_bridge_cmd_idx = 0U;
        }
      }
      else
      {
        dumb_bridge_cmd_idx = 0U;
        if (Buf[i] == (uint8_t)dumb_bridge_cmd[0])
        {
          dumb_bridge_cmd_idx = 1U;
        }
      }
    }
  }

  return (USBD_OK);
  /* USER CODE END 6 */
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
uint8_t CDC_Transmit_FS(uint8_t* Buf, uint16_t Len)
{
  uint8_t result = USBD_OK;
  /* USER CODE BEGIN 7 */
  USBD_CDC_HandleTypeDef *hcdc = (USBD_CDC_HandleTypeDef*)hUsbDeviceFS.pClassData;
  if (hcdc->TxState != 0){
    return USBD_BUSY;
  }
  USBD_CDC_SetTxBuffer(&hUsbDeviceFS, Buf, Len);
  result = USBD_CDC_TransmitPacket(&hUsbDeviceFS);
  /* USER CODE END 7 */
  return result;
}

/**
  * @brief  CDC_TransmitCplt_FS
  *         Data transmitted callback
  *
  *         @note
  *         This function is IN transfer complete callback used to inform user that
  *         the submitted Data is successfully sent over USB.
  *
  * @param  Buf: Buffer of data to be received
  * @param  Len: Number of data received (in bytes)
  * @retval Result of the operation: USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_TransmitCplt_FS(uint8_t *Buf, uint32_t *Len, uint8_t epnum)
{
  uint8_t result = USBD_OK;
  /* USER CODE BEGIN 13 */
  UNUSED(Buf);
  UNUSED(Len);
  UNUSED(epnum);
  cdc_tx_busy = 0U;
  /* USER CODE END 13 */
  return result;
}

/* USER CODE BEGIN PRIVATE_FUNCTIONS_IMPLEMENTATION */

static void Bridge_UartTxStart(void)
{
  if (uart_tx_active != 0U)
  {
    return;
  }
  if (uart_tx_head == uart_tx_tail)
  {
    return;
  }

  uart_tx_active = 1U;
  uart_tx_active_byte = uart_tx_buf[uart_tx_tail];
  HAL_UART_Transmit_IT(&huart3, &uart_tx_active_byte, 1U);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART3)
  {
    uint16_t next_head = (uart_rx_head + 1U) % CDC_BRIDGE_BUF_SIZE;
    if (next_head != uart_rx_tail)
    {
      uart_rx_buf[uart_rx_head] = uart_rx_byte;
      uart_rx_head = next_head;
    }
    HAL_UART_Receive_IT(&huart3, &uart_rx_byte, 1U);
  }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART3)
  {
    uart_tx_tail = (uart_tx_tail + 1U) % CDC_BRIDGE_BUF_SIZE;
    if (uart_tx_head != uart_tx_tail)
    {
      uart_tx_active_byte = uart_tx_buf[uart_tx_tail];
      HAL_UART_Transmit_IT(&huart3, &uart_tx_active_byte, 1U);
    }
    else
    {
      uart_tx_active = 0U;
    }
  }
}

void CDC_Bridge_Process(void)
{
  if (enter_bridge_request != 0U)
  {
    uint8_t do_validate = bridge_validate;
    enter_bridge_request = 0U;

    CDC_DebugPrintf("DBG: Step 1 - Reset main MCU into bootloader mode\r\n");
    MainMCU_ResetToBootloader();

    bridge_mode = 1U;
    cdc_tx_busy = 0U;
    uart_rx_head = 0U;
    uart_rx_tail = 0U;
    uart_tx_head = 0U;
    uart_tx_tail = 0U;
    uart_tx_active = 0U;

    HAL_NVIC_SetPriority(USART3_IRQn, 5U, 0U);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
    CDC_DebugPrintf("DBG: USART3 IRQ enabled\r\n");

    HAL_StatusTypeDef rx_status = HAL_UART_Receive_IT(&huart3, &uart_rx_byte, 1U);
    CDC_DebugPrintf("DBG: HAL_UART_Receive_IT status=%d\r\n", (int)rx_status);

    CDC_DebugPrintf("DBG: about to enter validation block (do_validate=%u)\r\n", (unsigned int)do_validate);

    if (do_validate != 0U)
    {
      CDC_DebugPrintf("DBG: Step 2 - Validate sync 0x7F -> 0x79\r\n");
      if (Bootloader_ValidateSync() != 0U)
      {
        CDC_DebugPrintf("DBG: Sync OK\r\n");
        CDC_SendString("BOOTLOADER OK\r\n");
      }
      else
      {
        CDC_DebugPrintf("DBG: Sync FAILED\r\n");
        CDC_SendString("BOOTLOADER FAILED\r\n");
        CDC_DebugPrintf("DBG: Resetting main MCU back to application mode\r\n");
        MainMCU_ResetToApp();
        bridge_mode = 0U;
      }
    }
    else
    {
      CDC_DebugPrintf("DBG: Bridge mode (no validation) - forwarding USB<->USART3\r\n");
      CDC_SendString("BRIDGE OK\r\n");
    }
  }

  if ((bridge_mode != 0U) && (cdc_tx_busy == 0U))
  {
    static uint8_t cdc_tx_buf[CDC_BRIDGE_BUF_SIZE];
    static uint16_t cdc_tx_len = 0U;

    if (cdc_tx_len == 0U)
    {
      while ((uart_rx_head != uart_rx_tail) && (cdc_tx_len < CDC_BRIDGE_BUF_SIZE))
      {
        cdc_tx_buf[cdc_tx_len++] = uart_rx_buf[uart_rx_tail];
        uart_rx_tail = (uart_rx_tail + 1U) % CDC_BRIDGE_BUF_SIZE;
      }
    }

    if (cdc_tx_len > 0U)
    {
      memcpy(UserTxBufferFS, cdc_tx_buf, cdc_tx_len);
      if (CDC_Transmit_FS(UserTxBufferFS, cdc_tx_len) == USBD_OK)
      {
        cdc_tx_busy = 1U;
        cdc_tx_len = 0U;
      }
    }
  }
}

uint8_t CDC_IsBridgeMode(void)
{
  return bridge_mode;
}

static void MainMCU_ResetToBootloader(void)
{
  /* BOOT0 must be sampled high when NRST rises (AN2606). Keep BOOTSEL high
     throughout the whole sequence and use generous settling/delays. */
  HAL_GPIO_WritePin(BOOTSEL_MAIN_MCU_GPIO_Port, BOOTSEL_MAIN_MCU_Pin, GPIO_PIN_SET);
  HAL_Delay(50);
  CDC_DebugPinStates("After BOOTSEL=H settle");

  HAL_GPIO_WritePin(RESET_MAIN_MCU_GPIO_Port, RESET_MAIN_MCU_Pin, GPIO_PIN_RESET);
  HAL_Delay(100);
  CDC_DebugPinStates("After RESET=L settle");

  HAL_GPIO_WritePin(RESET_MAIN_MCU_GPIO_Port, RESET_MAIN_MCU_Pin, GPIO_PIN_SET);
  HAL_Delay(300);
  CDC_DebugPinStates("After RESET=H settle");
}

static void MainMCU_ResetToApp(void)
{
  /* BOOT0 low, then reset so the main MCU boots from flash. */
  HAL_GPIO_WritePin(BOOTSEL_MAIN_MCU_GPIO_Port, BOOTSEL_MAIN_MCU_Pin, GPIO_PIN_RESET);
  HAL_Delay(50);
  CDC_DebugPinStates("After BOOTSEL=L settle");

  HAL_GPIO_WritePin(RESET_MAIN_MCU_GPIO_Port, RESET_MAIN_MCU_Pin, GPIO_PIN_RESET);
  HAL_Delay(100);
  CDC_DebugPinStates("After RESET=L settle");

  HAL_GPIO_WritePin(RESET_MAIN_MCU_GPIO_Port, RESET_MAIN_MCU_Pin, GPIO_PIN_SET);
  HAL_Delay(50);
  CDC_DebugPinStates("After RESET=H settle");
}

static uint8_t Bootloader_ValidateSync(void)
{
  uint8_t sync_byte = 0x7F;
  uint8_t ack_byte = 0;

  for (uint8_t attempt = 0U; attempt < 3U; attempt++)
  {
    CDC_DebugPrintf("DBG: Sync attempt %u, sending 0x7F\r\n", (unsigned int)(attempt + 1U));
    if (HAL_UART_Transmit(&huart3, &sync_byte, 1U, 100) != HAL_OK)
    {
      CDC_DebugPrintf("DBG: UART TX failed\r\n");
      HAL_Delay(50);
      continue;
    }

    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < 500U)
    {
      if (HAL_UART_Receive(&huart3, &ack_byte, 1U, 10) == HAL_OK)
      {
        CDC_DebugPrintf("DBG: RX byte 0x%02X\r\n", (unsigned int)ack_byte);
        if (ack_byte == 0x79U)
        {
          return 1U;
        }
      }
    }

    CDC_DebugPrintf("DBG: No ACK within 500 ms\r\n");
    HAL_Delay(50);
  }

  return 0U;
}

static void CDC_SendString(const char *str)
{
  uint16_t len = (uint16_t)strlen(str);
  if ((len == 0U) || (len > APP_TX_DATA_SIZE))
  {
    return;
  }

  memcpy(UserTxBufferFS, str, len);
  uint32_t start = HAL_GetTick();
  while (CDC_Transmit_FS(UserTxBufferFS, len) != USBD_OK)
  {
    if ((HAL_GetTick() - start) > 1000U)
    {
      break;
    }
  }
}

static void CDC_DebugPrintf(const char *fmt, ...)
{
  static char buf[128];
  va_list args;
  va_start(args, fmt);
  int len = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if ((len > 0) && (len < (int)sizeof(buf)))
  {
    CDC_SendString(buf);
  }
}

static void CDC_DebugPinStates(const char *label)
{
  GPIO_PinState bootsel = HAL_GPIO_ReadPin(BOOTSEL_MAIN_MCU_GPIO_Port, BOOTSEL_MAIN_MCU_Pin);
  GPIO_PinState reset = HAL_GPIO_ReadPin(RESET_MAIN_MCU_GPIO_Port, RESET_MAIN_MCU_Pin);

  CDC_DebugPrintf("DBG: %-24s BOOTSEL=%s RESET=%s\r\n",
                  label,
                  (bootsel == GPIO_PIN_SET) ? "H" : "L",
                  (reset == GPIO_PIN_SET) ? "H" : "L");
}

void CDC_DebugReportStartup(void)
{
  GPIO_PinState bootsel = HAL_GPIO_ReadPin(BOOTSEL_MAIN_MCU_GPIO_Port, BOOTSEL_MAIN_MCU_Pin);
  GPIO_PinState reset = HAL_GPIO_ReadPin(RESET_MAIN_MCU_GPIO_Port, RESET_MAIN_MCU_Pin);

  CDC_DebugPrintf("\r\n=== CoProcessor startup ===\r\n");
  CDC_DebugPrintf("DBG: Initial pin state        BOOTSEL=%s RESET=%s\r\n",
                  (bootsel == GPIO_PIN_SET) ? "H" : "L",
                  (reset == GPIO_PIN_SET) ? "H" : "L");
  CDC_DebugPrintf("DBG: Send 'BOOTLOADER' to flash the main MCU\r\n");
}

/* USER CODE END PRIVATE_FUNCTIONS_IMPLEMENTATION */

/**
  * @}
  */

/**
  * @}
  */
