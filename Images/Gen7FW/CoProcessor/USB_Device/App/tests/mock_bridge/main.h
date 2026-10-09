#ifndef TEST_BRIDGE_MAIN_H
#define TEST_BRIDGE_MAIN_H
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint32_t CR1, CR3, RQR, ICR, ISR, RDR, TDR; } TestUsart;
typedef struct { uint32_t CCR, CNDTR, CPAR, CMAR; } TestDmaChannel;
typedef struct { uint32_t ISR, IFCR; } TestDma;
typedef struct { uint32_t CCR; } TestMux;
extern TestUsart test_usart;
extern TestDmaChannel test_rx_channel, test_tx_channel;
extern TestDma test_dma;
extern TestMux test_rx_mux, test_tx_mux;
#define USART3 (&test_usart)
#define DMA1_Channel1 (&test_rx_channel)
#define DMA1_Channel2 (&test_tx_channel)
#define DMA1 (&test_dma)
#define DMAMUX1_Channel0 (&test_rx_mux)
#define DMAMUX1_Channel1 (&test_tx_mux)

enum {
    USART_ISR_PE = 1u, USART_ISR_FE = 2u, USART_ISR_NE = 4u, USART_ISR_ORE = 8u,
    USART_ICR_PECF = 1u, USART_ICR_FECF = 2u, USART_ICR_NECF = 4u, USART_ICR_ORECF = 8u,
    USART_CR1_TXEIE_TXFNFIE = 16u, USART_CR1_RXNEIE_RXFNEIE = 32u, USART_CR1_PEIE = 64u,
    USART_CR3_EIE = 128u, USART_CR3_DMAR = 256u, USART_CR3_DMAT = 512u,
    USART_RQR_RXFRQ = 1u,
    DMA_CCR_EN = 1u, DMA_CCR_MINC = 2u, DMA_CCR_CIRC = 4u,
    DMA_CCR_HTIE = 8u, DMA_CCR_TCIE = 16u, DMA_CCR_TEIE = 32u,
    DMA_CCR_PL_1 = 64u, DMA_CCR_DIR = 128u,
    DMA_ISR_HTIF1 = 1u, DMA_ISR_TCIF1 = 2u, DMA_ISR_TEIF1 = 4u,
    DMA_ISR_TCIF2 = 8u, DMA_ISR_TEIF2 = 16u,
    DMA_IFCR_CGIF1 = 1u, DMA_IFCR_CGIF2 = 2u,
    DMA_REQUEST_USART3_RX, DMA_REQUEST_USART3_TX,
    DMA1_Channel1_IRQn, DMA1_Channel2_IRQn, USART3_IRQn,
    UART_WORDLENGTH_9B, UART_WORDLENGTH_8B, UART_STOPBITS_1,
    UART_PARITY_EVEN, UART_PARITY_NONE, UART_MODE_TX_RX,
    UART_HWCONTROL_NONE, UART_OVERSAMPLING_16, UART_ONE_BIT_SAMPLE_DISABLE,
    UART_PRESCALER_DIV1, UART_ADVFEATURE_NO_INIT,
    UART_TXFIFO_THRESHOLD_1_8, UART_RXFIFO_THRESHOLD_1_8
};
typedef enum { HAL_OK, HAL_ERROR } HAL_StatusTypeDef;
typedef struct {
    TestUsart *Instance;
    struct {
        uint32_t BaudRate, WordLength, StopBits, Parity, Mode, HwFlowCtl;
        uint32_t OverSampling, OneBitSampling, ClockPrescaler;
    } Init;
    struct { uint32_t AdvFeatureInit; } AdvancedInit;
} UART_HandleTypeDef;
typedef enum { GPIO_PIN_RESET, GPIO_PIN_SET } GPIO_PinState;
#define BOOTSEL_MAIN_MCU_GPIO_Port 0u
#define BOOTSEL_MAIN_MCU_Pin 0u
#define RESET_MAIN_MCU_GPIO_Port 0u
#define RESET_MAIN_MCU_Pin 1u
#define __get_PRIMASK() (0u)
#define __disable_irq() ((void)0)
#define __enable_irq() ((void)0)
#define __DMB() ((void)0)
#define __HAL_RCC_DMA1_CLK_ENABLE() ((void)0)
#define __HAL_RCC_DMAMUX1_CLK_ENABLE() ((void)0)
static inline void HAL_NVIC_ClearPendingIRQ(unsigned n) { (void)n; }
static inline void HAL_NVIC_DisableIRQ(unsigned n) { (void)n; }
static inline void HAL_NVIC_EnableIRQ(unsigned n) { (void)n; }
static inline void HAL_NVIC_SetPriority(unsigned n, unsigned p, unsigned s) { (void)n; (void)p; (void)s; }
static inline HAL_StatusTypeDef HAL_UART_DeInit(UART_HandleTypeDef *u) { (void)u; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef *u) { (void)u; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_UARTEx_SetTxFifoThreshold(UART_HandleTypeDef *u, unsigned t) { (void)u; (void)t; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_UARTEx_SetRxFifoThreshold(UART_HandleTypeDef *u, unsigned t) { (void)u; (void)t; return HAL_OK; }
static inline HAL_StatusTypeDef HAL_UARTEx_EnableFifoMode(UART_HandleTypeDef *u) { (void)u; return HAL_OK; }
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t ms);
void HAL_GPIO_WritePin(unsigned port, unsigned pin, GPIO_PinState state);
GPIO_PinState HAL_GPIO_ReadPin(unsigned port, unsigned pin);

/* Skip the MCU USB interface header while compiling its real implementation. */
#define __USBD_CDC_IF_H__
#define APP_RX_DATA_SIZE 2048u
#define APP_TX_DATA_SIZE 2048u
#define CDC_BRIDGE_BUF_SIZE 2048u
#define CDC_DATA_FS_MAX_PACKET_SIZE 64u
enum { CDC_PORT_COUNT = 2, CDC_PORT_BRIDGE = 0, CDC_PORT_CONTROL = 1,
       USBD_OK = 0, USBD_BUSY = 1, USBD_FAIL = 2, CDC_GET_LINE_CODING = 0x21 };
typedef struct { unsigned unused; } USBD_HandleTypeDef;
typedef struct {
    int8_t (*Init)(uint8_t);
    int8_t (*DeInit)(uint8_t);
    int8_t (*Control)(uint8_t, uint8_t, uint8_t*, uint16_t);
    int8_t (*Receive)(uint8_t, uint8_t*, uint32_t*);
    int8_t (*TransmitCplt)(uint8_t, uint8_t*, uint32_t*, uint8_t);
} USBD_CDC_ItfTypeDef;
uint8_t USBD_CDC_SetTxBuffer(USBD_HandleTypeDef*, uint8_t, uint8_t*, uint32_t);
uint8_t USBD_CDC_SetRxBuffer(USBD_HandleTypeDef*, uint8_t, uint8_t*);
uint8_t USBD_CDC_TransmitPacket(USBD_HandleTypeDef*, uint8_t);
uint8_t USBD_CDC_ReceivePacket(USBD_HandleTypeDef*, uint8_t);
uint8_t CDC_Transmit_FS(uint8_t, uint8_t*, uint16_t);
#endif
