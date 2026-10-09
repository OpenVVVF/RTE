#pragma once
#include "../mock/main.h"

typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY } HAL_StatusTypeDef;
typedef struct { unsigned RxState; void* Instance; } UART_HandleTypeDef;
#define USART3 ((void*)0x1234u)
enum {
    HAL_UART_STATE_READY,
    HAL_UART_STATE_BUSY_RX,
    UART_RXDATA_FLUSH_REQUEST,
    UART_CLEAR_PEF = 1u << 4,
    UART_CLEAR_FEF = 1u << 5,
    UART_CLEAR_NEF = 1u << 6,
    UART_CLEAR_OREF = 1u << 7,
    UART_CLEAR_IDLEF = 1u << 8,
    USART3_IRQn
};
void test_uart_flush(void);
#define __HAL_UART_SEND_REQ(uart, request) test_uart_flush()
#define __HAL_UART_CLEAR_FLAG(uart, flags) ((void)(flags))
#define __enable_irq() ((void)0)
inline void HAL_NVIC_ClearPendingIRQ(unsigned) {}
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef*, uint8_t*, uint16_t);
