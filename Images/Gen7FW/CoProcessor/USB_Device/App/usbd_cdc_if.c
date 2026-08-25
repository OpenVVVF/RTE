#include "usbd_cdc_if.h"
#include <stdio.h>
#include <string.h>
#include "main.h"
#include "usart.h"

extern USBD_HandleTypeDef hUsbDeviceFS;
uint8_t UserRxBufferFS[CDC_PORT_COUNT][APP_RX_DATA_SIZE];
uint8_t UserTxBufferFS[CDC_PORT_COUNT][APP_TX_DATA_SIZE];
static uint8_t uart_rx_ring[CDC_BRIDGE_BUF_SIZE], uart_tx_ring[CDC_BRIDGE_BUF_SIZE];
static volatile uint16_t uart_rx_head, uart_rx_tail, uart_tx_head, uart_tx_tail;
static volatile uint8_t usb_tx_busy[CDC_PORT_COUNT];
static volatile uint8_t bridge_out_armed;
static volatile uint8_t bridge_paused;
static volatile uint8_t uart_bootloader_mode;
static volatile uint32_t uart_errors, uart_rx_dropped, uart_tx_dropped;
static uint8_t command[32], command_length;
static volatile uint8_t requested_action;
enum { ACTION_NONE, ACTION_BOOTLOADER, ACTION_APP, ACTION_RESET, ACTION_STATUS };

static int8_t cdc_init(uint8_t), cdc_deinit(uint8_t);
static int8_t cdc_control(uint8_t, uint8_t, uint8_t *, uint16_t);
static int8_t cdc_receive(uint8_t, uint8_t *, uint32_t *);
static int8_t cdc_tx_complete(uint8_t, uint8_t *, uint32_t *, uint8_t);
static void uart_tx_start(void), bridge_out_arm_if_ready(void), reset_main(GPIO_PinState), send_control(const char *);
static uint8_t uart_set_bootloader_mode(uint8_t bootloader);
static void uart_clear_bridge_buffers(void);
USBD_CDC_ItfTypeDef USBD_Interface_fops_FS = {cdc_init,cdc_deinit,cdc_control,cdc_receive,cdc_tx_complete};

static int8_t cdc_init(uint8_t p){USBD_CDC_SetTxBuffer(&hUsbDeviceFS,p,UserTxBufferFS[p],0);USBD_CDC_SetRxBuffer(&hUsbDeviceFS,p,UserRxBufferFS[p]);if(p==CDC_PORT_BRIDGE){bridge_out_armed=1;USART3->CR1|=USART_CR1_RXNEIE_RXFNEIE|USART_CR1_PEIE;USART3->CR3|=USART_CR3_EIE;HAL_NVIC_SetPriority(USART3_IRQn,5,0);HAL_NVIC_EnableIRQ(USART3_IRQn);}return USBD_OK;}
static int8_t cdc_deinit(uint8_t p){(void)p;return USBD_OK;}
static int8_t cdc_control(uint8_t p,uint8_t cmd,uint8_t*b,uint16_t n){(void)p;if(cmd==CDC_GET_LINE_CODING&&n>=7){b[0]=0x00;b[1]=0x08;b[2]=0x07;b[3]=0x00;b[4]=0;b[5]=0;b[6]=8;}return USBD_OK;}

static int8_t cdc_receive(uint8_t p,uint8_t*b,uint32_t*n){
 if(p==CDC_PORT_BRIDGE){bridge_out_armed=0;if(bridge_paused){uart_tx_dropped+=*n;}else for(uint32_t i=0;i<*n;i++){uint16_t x=(uart_tx_head+1U)%CDC_BRIDGE_BUF_SIZE;if(x==uart_tx_tail){uart_tx_dropped++;break;}uart_tx_ring[uart_tx_head]=b[i];uart_tx_head=x;}uart_tx_start();bridge_out_arm_if_ready();return USBD_OK;}
 else for(uint32_t i=0;i<*n;i++){uint8_t c=b[i];if(c=='\r'||c=='\n'){if(command_length){command[command_length]=0;if(!strcmp((char*)command,"BOOTLOADER"))requested_action=ACTION_BOOTLOADER;else if(!strcmp((char*)command,"APP"))requested_action=ACTION_APP;else if(!strcmp((char*)command,"RESET"))requested_action=ACTION_RESET;else if(!strcmp((char*)command,"STATUS"))requested_action=ACTION_STATUS;else send_control("ERROR UNKNOWN COMMAND\r\n");command_length=0;}}else if(command_length<sizeof(command)-1U)command[command_length++]=c;else command_length=0;}
 USBD_CDC_SetRxBuffer(&hUsbDeviceFS,p,UserRxBufferFS[p]);USBD_CDC_ReceivePacket(&hUsbDeviceFS,p);return USBD_OK;}
static int8_t cdc_tx_complete(uint8_t p,uint8_t*b,uint32_t*n,uint8_t e){(void)b;(void)n;(void)e;usb_tx_busy[p]=0;return USBD_OK;}
uint8_t CDC_Transmit_FS(uint8_t p,uint8_t*b,uint16_t n){if(p>=2||usb_tx_busy[p])return USBD_BUSY;USBD_CDC_SetTxBuffer(&hUsbDeviceFS,p,b,n);if(USBD_CDC_TransmitPacket(&hUsbDeviceFS,p)!=USBD_OK)return USBD_BUSY;usb_tx_busy[p]=1;return USBD_OK;}
static void uart_tx_start(void){if(!bridge_paused&&uart_tx_head!=uart_tx_tail)USART3->CR1|=USART_CR1_TXEIE_TXFNFIE;}

/* Switch framing only from the main loop. USB callbacks may preempt this code,
 * so bridge_paused prevents them from touching USART3 while HAL tears it down.
 * Application mode is 460800 8N1; the STM32 ROM bootloader requires 8E1. */
static uint8_t uart_set_bootloader_mode(uint8_t bootloader){
 bridge_paused=1;
 HAL_NVIC_DisableIRQ(USART3_IRQn);
 USART3->CR1&=~(USART_CR1_TXEIE_TXFNFIE|USART_CR1_RXNEIE_RXFNEIE|USART_CR1_PEIE);
 USART3->CR3&=~USART_CR3_EIE;
 uart_rx_head=uart_rx_tail=0;
 uart_tx_head=uart_tx_tail=0;

 if(HAL_UART_DeInit(&huart3)!=HAL_OK)goto fail;
 huart3.Instance=USART3;
 huart3.Init.BaudRate=460800;
 huart3.Init.WordLength=bootloader?UART_WORDLENGTH_9B:UART_WORDLENGTH_8B;
 huart3.Init.StopBits=UART_STOPBITS_1;
 huart3.Init.Parity=bootloader?UART_PARITY_EVEN:UART_PARITY_NONE;
 huart3.Init.Mode=UART_MODE_TX_RX;
 huart3.Init.HwFlowCtl=UART_HWCONTROL_NONE;
 huart3.Init.OverSampling=UART_OVERSAMPLING_16;
 huart3.Init.OneBitSampling=UART_ONE_BIT_SAMPLE_DISABLE;
 huart3.Init.ClockPrescaler=UART_PRESCALER_DIV1;
 huart3.AdvancedInit.AdvFeatureInit=UART_ADVFEATURE_NO_INIT;
 if(HAL_UART_Init(&huart3)!=HAL_OK)goto fail;
 if(HAL_UARTEx_SetTxFifoThreshold(&huart3,UART_TXFIFO_THRESHOLD_1_8)!=HAL_OK)goto fail;
 if(HAL_UARTEx_SetRxFifoThreshold(&huart3,UART_RXFIFO_THRESHOLD_1_8)!=HAL_OK)goto fail;
 if(HAL_UARTEx_DisableFifoMode(&huart3)!=HAL_OK)goto fail;

 USART3->RQR=USART_RQR_RXFRQ;
 USART3->ICR=USART_ICR_PECF|USART_ICR_FECF|USART_ICR_NECF|USART_ICR_ORECF;
 USART3->CR1|=USART_CR1_RXNEIE_RXFNEIE|USART_CR1_PEIE;
 USART3->CR3|=USART_CR3_EIE;
 HAL_NVIC_ClearPendingIRQ(USART3_IRQn);
 HAL_NVIC_SetPriority(USART3_IRQn,5,0);
 HAL_NVIC_EnableIRQ(USART3_IRQn);
 uart_bootloader_mode=bootloader?1U:0U;
 bridge_paused=0;
 return 1;

fail:
 uart_errors++;
 /* Keep the data bridge paused after a failed HAL reinitialization. The
  * control CDC port remains usable for diagnosis and another mode command. */
 bridge_paused=1;
 return 0;
}

static void uart_clear_bridge_buffers(void){
 bridge_paused=1;
 HAL_NVIC_DisableIRQ(USART3_IRQn);
 USART3->CR1&=~USART_CR1_TXEIE_TXFNFIE;
 uart_rx_head=uart_rx_tail=0;
 uart_tx_head=uart_tx_tail=0;
 USART3->RQR=USART_RQR_RXFRQ;
 USART3->ICR=USART_ICR_PECF|USART_ICR_FECF|USART_ICR_NECF|USART_ICR_ORECF;
 HAL_NVIC_ClearPendingIRQ(USART3_IRQn);
 HAL_NVIC_EnableIRQ(USART3_IRQn);
 bridge_paused=0;
}
static void bridge_out_arm_if_ready(void){uint16_t used=(uart_tx_head+CDC_BRIDGE_BUF_SIZE-uart_tx_tail)%CDC_BRIDGE_BUF_SIZE;uint16_t free_space=CDC_BRIDGE_BUF_SIZE-1U-used;if(!bridge_out_armed&&free_space>=CDC_DATA_FS_MAX_PACKET_SIZE){/* Publish the armed state before enabling EP1 OUT. A packet already pending in the host can complete immediately and its callback must be allowed to change the state back to zero. */bridge_out_armed=1;USBD_CDC_SetRxBuffer(&hUsbDeviceFS,CDC_PORT_BRIDGE,UserRxBufferFS[CDC_PORT_BRIDGE]);if(USBD_CDC_ReceivePacket(&hUsbDeviceFS,CDC_PORT_BRIDGE)!=USBD_OK)bridge_out_armed=0;}}
void CDC_UartIrqHandler(void){uint32_t status=USART3->ISR;uint32_t errors=status&(USART_ISR_PE|USART_ISR_FE|USART_ISR_NE|USART_ISR_ORE);if(errors){uart_errors++;USART3->ICR=USART_ICR_PECF|USART_ICR_FECF|USART_ICR_NECF|USART_ICR_ORECF;if(status&USART_ISR_RXNE_RXFNE)(void)USART3->RDR;}else if(status&USART_ISR_RXNE_RXFNE){uint8_t byte=(uint8_t)USART3->RDR;uint16_t next=(uart_rx_head+1U)%CDC_BRIDGE_BUF_SIZE;if(next!=uart_rx_tail){uart_rx_ring[uart_rx_head]=byte;uart_rx_head=next;}else uart_rx_dropped++;}if((USART3->CR1&USART_CR1_TXEIE_TXFNFIE)&&(USART3->ISR&USART_ISR_TXE_TXFNF)){if(uart_tx_head!=uart_tx_tail){USART3->TDR=uart_tx_ring[uart_tx_tail];uart_tx_tail=(uart_tx_tail+1U)%CDC_BRIDGE_BUF_SIZE;}else USART3->CR1&=~USART_CR1_TXEIE_TXFNFIE;}}
static void reset_main(GPIO_PinState boot){HAL_GPIO_WritePin(BOOTSEL_MAIN_MCU_GPIO_Port,BOOTSEL_MAIN_MCU_Pin,boot);HAL_Delay(20);HAL_GPIO_WritePin(RESET_MAIN_MCU_GPIO_Port,RESET_MAIN_MCU_Pin,GPIO_PIN_RESET);HAL_Delay(50);HAL_GPIO_WritePin(RESET_MAIN_MCU_GPIO_Port,RESET_MAIN_MCU_Pin,GPIO_PIN_SET);HAL_Delay(100);HAL_GPIO_WritePin(BOOTSEL_MAIN_MCU_GPIO_Port,BOOTSEL_MAIN_MCU_Pin,GPIO_PIN_RESET);}
static void send_control(const char*s){size_t n=strlen(s);if(n>APP_TX_DATA_SIZE)return;memcpy(UserTxBufferFS[1],s,n);CDC_Transmit_FS(1,UserTxBufferFS[1],(uint16_t)n);}
void CDC_Bridge_Process(void){bridge_out_arm_if_ready();uint8_t a=requested_action;if(a){requested_action=0;if(a==ACTION_BOOTLOADER){if(uart_set_bootloader_mode(1)){reset_main(GPIO_PIN_SET);uart_clear_bridge_buffers();send_control("BOOTLOADER OK\r\n");}else send_control("ERROR UART CONFIG\r\n");}else if(a==ACTION_APP){if(uart_set_bootloader_mode(0)){reset_main(GPIO_PIN_RESET);uart_clear_bridge_buffers();send_control("APP OK\r\n");}else send_control("ERROR UART CONFIG\r\n");}else if(a==ACTION_RESET){if(uart_set_bootloader_mode(0)){reset_main(GPIO_PIN_RESET);uart_clear_bridge_buffers();send_control("RESET OK\r\n");}else send_control("ERROR UART CONFIG\r\n");}else{char s[160];snprintf(s,sizeof(s),"STATUS BOOTSEL=%u RESET=%u UART_MODE=%s UART_ERRORS=%lu RX_DROPPED=%lu TX_DROPPED=%lu\r\n",HAL_GPIO_ReadPin(BOOTSEL_MAIN_MCU_GPIO_Port,BOOTSEL_MAIN_MCU_Pin),HAL_GPIO_ReadPin(RESET_MAIN_MCU_GPIO_Port,RESET_MAIN_MCU_Pin),uart_bootloader_mode?"BOOT_8E1":"APP_8N1",(unsigned long)uart_errors,(unsigned long)uart_rx_dropped,(unsigned long)uart_tx_dropped);send_control(s);}}
 if(!usb_tx_busy[0]&&uart_rx_head!=uart_rx_tail){uint16_t n=0;while(uart_rx_head!=uart_rx_tail&&n<APP_TX_DATA_SIZE){UserTxBufferFS[0][n++]=uart_rx_ring[uart_rx_tail];uart_rx_tail=(uart_rx_tail+1U)%CDC_BRIDGE_BUF_SIZE;}CDC_Transmit_FS(0,UserTxBufferFS[0],n);}}
uint8_t CDC_IsBridgeMode(void){return 1;}void CDC_DebugReportStartup(void){if(uart_set_bootloader_mode(0))reset_main(GPIO_PIN_RESET);}
