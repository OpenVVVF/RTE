/* usart.h — SIL CubeMX-style header: UART peripheral handle externs. */
#ifndef SIL_USART_H
#define SIL_USART_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

extern UART_HandleTypeDef huart3;

void MX_USART3_UART_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* SIL_USART_H */
