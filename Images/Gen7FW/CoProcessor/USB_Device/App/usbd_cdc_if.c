#include "usbd_cdc_if.h"
#include <stdio.h>
#include <string.h>
#include "main.h"
#include "usart.h"
#include "safety_hardware.h"
#include "bridge_dma_math.h"

extern USBD_HandleTypeDef hUsbDeviceFS;
uint8_t UserRxBufferFS[CDC_PORT_COUNT][APP_RX_DATA_SIZE];
uint8_t UserTxBufferFS[CDC_PORT_COUNT][APP_TX_DATA_SIZE];
/* USART3 RX runs continuously into DMA1 channel 1. Channel 2 drains the
 * USB-to-UART ring in blocks, so normal telemetry needs no per-byte ISR. */
static uint8_t uart_rx_dma[BRIDGE_RX_DMA_SIZE], uart_tx_ring[CDC_BRIDGE_BUF_SIZE];
static volatile uint32_t uart_rx_completed_halves;
static volatile uint8_t uart_rx_restart_pending;
static uint32_t uart_rx_consumed, uart_rx_scanned;
static volatile uint16_t uart_tx_head, uart_tx_tail;
static volatile uint8_t uart_tx_dma_active, uart_tx_dma_safety;
static volatile uint16_t uart_tx_dma_length;
static volatile uint8_t uart_tx_dma_last_byte;
static volatile uint8_t usb_tx_busy[CDC_PORT_COUNT];
static volatile uint8_t bridge_out_armed;
static volatile uint8_t bridge_paused;
static volatile uint8_t uart_bootloader_mode;
static volatile uint8_t bridge_waiting_for_app;
static volatile uint8_t bridge_reset_active;
static volatile uint32_t bridge_app_wait_since;
static volatile uint32_t uart_errors, uart_rx_dropped, uart_tx_dropped;
static uint32_t bridge_rx_last_poll_ms = UINT32_MAX;
static uint8_t command[32], command_length;
static volatile uint8_t requested_action;
/* Status lines share the G474 -> H7 UART with USB commands. Keep a separate
 * complete frame so an ISR never inserts it in the middle of a host line. */
static char safety_pending[SAFETY_FAULT_WIRE_LENGTH];
static char safety_active_frame[SAFETY_FAULT_WIRE_LENGTH];
static volatile uint8_t safety_pending_valid;
static volatile uint8_t safety_active;
static uint8_t tx_at_line_boundary = 1u;
enum { ACTION_NONE, ACTION_BOOTLOADER, ACTION_APP, ACTION_RESET, ACTION_STATUS };

static int8_t cdc_init(uint8_t), cdc_deinit(uint8_t);
static int8_t cdc_control(uint8_t, uint8_t, uint8_t *, uint16_t);
static int8_t cdc_receive(uint8_t, uint8_t *, uint32_t *);
static int8_t cdc_tx_complete(uint8_t, uint8_t *, uint32_t *, uint8_t);
static void uart_tx_start(void), bridge_out_arm_if_ready(void), reset_main(GPIO_PinState), send_control(const char *);
static uint8_t uart_set_bootloader_mode(uint8_t bootloader);
static void uart_clear_bridge_buffers(void);
static void uart_dma_stop(void), uart_dma_start(void);
static uint32_t uart_rx_produced(void);
USBD_CDC_ItfTypeDef USBD_Interface_fops_FS = {cdc_init,cdc_deinit,cdc_control,cdc_receive,cdc_tx_complete};

static int8_t cdc_init(uint8_t p){usb_tx_busy[p]=0;USBD_CDC_SetTxBuffer(&hUsbDeviceFS,p,UserTxBufferFS[p],0);USBD_CDC_SetRxBuffer(&hUsbDeviceFS,p,UserRxBufferFS[p]);if(p==CDC_PORT_BRIDGE)bridge_out_armed=1;return USBD_OK;}
static int8_t cdc_deinit(uint8_t p){usb_tx_busy[p]=0;return USBD_OK;}
static int8_t cdc_control(uint8_t p,uint8_t cmd,uint8_t*b,uint16_t n){
 if(cmd==CDC_GET_LINE_CODING&&n>=7){
  const uint32_t baud=p==CDC_PORT_CONTROL?115200U:(uart_bootloader_mode?460800U:921600U);
  b[0]=(uint8_t)baud;b[1]=(uint8_t)(baud>>8);b[2]=(uint8_t)(baud>>16);b[3]=(uint8_t)(baud>>24);
  b[4]=0;b[5]=uart_bootloader_mode&&p==CDC_PORT_BRIDGE?2:0;b[6]=8;
 }
 return USBD_OK;
}

static int8_t cdc_receive(uint8_t p,uint8_t*b,uint32_t*n){
 if(p==CDC_PORT_BRIDGE){
  bridge_out_armed=0;
  const uint16_t head=uart_tx_head;
  const uint16_t used=(head+CDC_BRIDGE_BUF_SIZE-uart_tx_tail)%CDC_BRIDGE_BUF_SIZE;
  const uint16_t free_space=CDC_BRIDGE_BUF_SIZE-1U-used;
  const uint16_t count=(uint16_t)((*n<free_space)?*n:free_space);
  const uint16_t first=(count<CDC_BRIDGE_BUF_SIZE-head)?count:CDC_BRIDGE_BUF_SIZE-head;
  memcpy(&uart_tx_ring[head],b,first);
  memcpy(uart_tx_ring,b+first,count-first);
  uart_tx_head=(head+count)%CDC_BRIDGE_BUF_SIZE;
  uart_tx_dropped+=*n-count;
  uart_tx_start();bridge_out_arm_if_ready();return USBD_OK;
 }
 else for(uint32_t i=0;i<*n;i++){uint8_t c=b[i];if(c=='\r'||c=='\n'){if(command_length){command[command_length]=0;if(!strcmp((char*)command,"BOOTLOADER"))requested_action=ACTION_BOOTLOADER;else if(!strcmp((char*)command,"APP"))requested_action=ACTION_APP;else if(!strcmp((char*)command,"RESET"))requested_action=ACTION_RESET;else if(!strcmp((char*)command,"STATUS"))requested_action=ACTION_STATUS;else send_control("ERROR UNKNOWN COMMAND\r\n");command_length=0;}}else if(command_length<sizeof(command)-1U)command[command_length++]=c;else command_length=0;}
 USBD_CDC_SetRxBuffer(&hUsbDeviceFS,p,UserRxBufferFS[p]);USBD_CDC_ReceivePacket(&hUsbDeviceFS,p);return USBD_OK;}
static int8_t cdc_tx_complete(uint8_t p,uint8_t*b,uint32_t*n,uint8_t e){(void)b;(void)n;(void)e;usb_tx_busy[p]=0;return USBD_OK;}
uint8_t CDC_Transmit_FS(uint8_t p,uint8_t*b,uint16_t n){
 if(p>=2||usb_tx_busy[p])return USBD_BUSY;
 /* Publish busy before starting USB transfer; completion may interrupt us. */
 usb_tx_busy[p]=1;
 uint8_t status=USBD_CDC_SetTxBuffer(&hUsbDeviceFS,p,b,n);
 if(status==USBD_OK)status=USBD_CDC_TransmitPacket(&hUsbDeviceFS,p);
 if(status!=USBD_OK)usb_tx_busy[p]=0;
 return status;
}
static void uart_tx_start(void){
 const uint32_t irq_state=__get_PRIMASK();
 __disable_irq();
 if(!bridge_paused&&!bridge_waiting_for_app&&!uart_tx_dma_active){
  const uint8_t *source=NULL;
  uint16_t length=0u;
  if(!uart_bootloader_mode&&safety_pending_valid&&tx_at_line_boundary){
   memcpy(safety_active_frame,safety_pending,sizeof(safety_active_frame));
   safety_pending_valid=0u;
   safety_active=1u;
   uart_tx_dma_safety=1u;
   source=(const uint8_t *)safety_active_frame;
   length=SAFETY_FAULT_WIRE_LENGTH;
  }else if(uart_tx_head!=uart_tx_tail){
   const uint16_t tail=uart_tx_tail;
   const uint16_t contiguous=uart_tx_head>tail?uart_tx_head-tail:CDC_BRIDGE_BUF_SIZE-tail;
   length=contiguous;
   length=BridgeDma_TxChunk(&uart_tx_ring[tail],contiguous,uart_bootloader_mode);
   uart_tx_dma_safety=0u;
   uart_tx_dma_last_byte=uart_tx_ring[tail+length-1U];
   source=&uart_tx_ring[tail];
  }
  if(length){
   DMA1_Channel2->CCR&=~DMA_CCR_EN;
   DMA1->IFCR=DMA_IFCR_CGIF2;
   DMA1_Channel2->CNDTR=length;
   DMA1_Channel2->CMAR=(uint32_t)(uintptr_t)source;
   uart_tx_dma_length=length;
   uart_tx_dma_active=1u;
   __DMB();
   DMA1_Channel2->CCR|=DMA_CCR_EN;
  }
 }
 if(!irq_state)__enable_irq();
}

void CDC_Bridge_QueueSafetyStatus(const SafetyFaultFrame *frame){
 if(frame==NULL||uart_bootloader_mode)return;
 char encoded[SAFETY_FAULT_WIRE_LENGTH];
 SafetyFaultWire_Encode(frame,encoded);
 const uint32_t irq_state=__get_PRIMASK();
 __disable_irq();
 safety_pending_valid=0u;
 memcpy(safety_pending,encoded,sizeof(safety_pending));
 safety_pending_valid=1u;
 if(!irq_state)__enable_irq();
 uart_tx_start();
}

/* Switch framing only from the main loop. USB callbacks may preempt this code,
 * so bridge_paused prevents them from touching USART3 while HAL tears it down.
 * Application mode is 921600 8N1; the STM32 ROM bootloader stays 460800 8E1. */
static void uart_dma_stop(void){
 HAL_NVIC_DisableIRQ(DMA1_Channel1_IRQn);
 HAL_NVIC_DisableIRQ(DMA1_Channel2_IRQn);
 USART3->CR3&=~(USART_CR3_DMAR|USART_CR3_DMAT);
 DMA1_Channel1->CCR&=~DMA_CCR_EN;
 DMA1_Channel2->CCR&=~DMA_CCR_EN;
 DMA1->IFCR=DMA_IFCR_CGIF1|DMA_IFCR_CGIF2;
 uart_tx_dma_active=0u;
 uart_tx_dma_safety=0u;
 uart_tx_dma_length=0u;
 uart_rx_restart_pending=0u;
}

static void uart_dma_start(void){
 __HAL_RCC_DMA1_CLK_ENABLE();
 __HAL_RCC_DMAMUX1_CLK_ENABLE();
 DMA1_Channel1->CCR=0u;
 DMA1_Channel2->CCR=0u;
 DMA1->IFCR=DMA_IFCR_CGIF1|DMA_IFCR_CGIF2;
 DMAMUX1_Channel0->CCR=DMA_REQUEST_USART3_RX;
 DMAMUX1_Channel1->CCR=DMA_REQUEST_USART3_TX;
 uart_rx_completed_halves=0u;
 uart_rx_consumed=0u;
 uart_rx_scanned=0u;
 bridge_rx_last_poll_ms=UINT32_MAX;
 uart_rx_restart_pending=0u;
 DMA1_Channel1->CPAR=(uint32_t)(uintptr_t)&USART3->RDR;
 DMA1_Channel1->CMAR=(uint32_t)(uintptr_t)uart_rx_dma;
 DMA1_Channel1->CNDTR=BRIDGE_RX_DMA_SIZE;
 DMA1_Channel1->CCR=DMA_CCR_MINC|DMA_CCR_CIRC|DMA_CCR_HTIE|
                    DMA_CCR_TCIE|DMA_CCR_TEIE|DMA_CCR_PL_1;
 DMA1_Channel2->CPAR=(uint32_t)(uintptr_t)&USART3->TDR;
 DMA1_Channel2->CCR=DMA_CCR_MINC|DMA_CCR_DIR|DMA_CCR_TCIE|
                    DMA_CCR_TEIE|DMA_CCR_PL_1;
 HAL_NVIC_ClearPendingIRQ(DMA1_Channel1_IRQn);
 HAL_NVIC_ClearPendingIRQ(DMA1_Channel2_IRQn);
 HAL_NVIC_SetPriority(DMA1_Channel1_IRQn,5,0);
 HAL_NVIC_SetPriority(DMA1_Channel2_IRQn,5,0);
 HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
 HAL_NVIC_EnableIRQ(DMA1_Channel2_IRQn);
 DMA1_Channel1->CCR|=DMA_CCR_EN;
 USART3->CR3|=USART_CR3_DMAR|USART_CR3_DMAT|USART_CR3_EIE;
}

static uint8_t uart_set_bootloader_mode(uint8_t bootloader){
 __HAL_RCC_DMA1_CLK_ENABLE();
 __HAL_RCC_DMAMUX1_CLK_ENABLE();
 uint32_t irq_state=__get_PRIMASK();
 __disable_irq();
 bridge_paused=1;
 HAL_NVIC_DisableIRQ(USART3_IRQn);
 USART3->CR1&=~(USART_CR1_TXEIE_TXFNFIE|USART_CR1_RXNEIE_RXFNEIE|USART_CR1_PEIE);
 USART3->CR3&=~USART_CR3_EIE;
 uart_dma_stop();
 uart_tx_head=uart_tx_tail=0;
 safety_pending_valid=0u;
 safety_active=0u;
 tx_at_line_boundary=1u;
 if(!irq_state)__enable_irq();

 if(HAL_UART_DeInit(&huart3)!=HAL_OK)goto fail;
 huart3.Instance=USART3;
 huart3.Init.BaudRate=bootloader?460800U:921600U;
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
 if(HAL_UARTEx_EnableFifoMode(&huart3)!=HAL_OK)goto fail;

 USART3->RQR=USART_RQR_RXFRQ;
 USART3->ICR=USART_ICR_PECF|USART_ICR_FECF|USART_ICR_NECF|USART_ICR_ORECF;
 uart_dma_start();
 USART3->CR1|=USART_CR1_PEIE;
 HAL_NVIC_ClearPendingIRQ(USART3_IRQn);
 HAL_NVIC_SetPriority(USART3_IRQn,5,0);
 HAL_NVIC_EnableIRQ(USART3_IRQn);
 uart_bootloader_mode=bootloader?1U:0U;
 /* A reset follows each mode switch. Hold USB commands until the application
  * emits a UART frame delimiter after reset. ROM flashing stays live. */
 bridge_waiting_for_app=bootloader?0U:1U;
 bridge_reset_active=bootloader?0U:1U;
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
 const uint32_t irq_state=__get_PRIMASK();
 __disable_irq();
 bridge_paused=1;
 HAL_NVIC_DisableIRQ(USART3_IRQn);
 uart_dma_stop();
 uart_tx_head=uart_tx_tail=0;
 USART3->RQR=USART_RQR_RXFRQ;
 USART3->ICR=USART_ICR_PECF|USART_ICR_FECF|USART_ICR_NECF|USART_ICR_ORECF;
 uart_dma_start();
 HAL_NVIC_ClearPendingIRQ(USART3_IRQn);
 HAL_NVIC_EnableIRQ(USART3_IRQn);
 bridge_paused=0;
 if(!irq_state)__enable_irq();
}
static void bridge_out_arm_if_ready(void){uint16_t used=(uart_tx_head+CDC_BRIDGE_BUF_SIZE-uart_tx_tail)%CDC_BRIDGE_BUF_SIZE;uint16_t free_space=CDC_BRIDGE_BUF_SIZE-1U-used;if(!bridge_out_armed&&free_space>=CDC_DATA_FS_MAX_PACKET_SIZE){/* Publish the armed state before enabling EP1 OUT. A packet already pending in the host can complete immediately and its callback must be allowed to change the state back to zero. */bridge_out_armed=1;USBD_CDC_SetRxBuffer(&hUsbDeviceFS,CDC_PORT_BRIDGE,UserRxBufferFS[CDC_PORT_BRIDGE]);if(USBD_CDC_ReceivePacket(&hUsbDeviceFS,CDC_PORT_BRIDGE)!=USBD_OK)bridge_out_armed=0;}}
void CDC_UartIrqHandler(void){
 const uint32_t status=USART3->ISR;
 const uint32_t errors=status&(USART_ISR_PE|USART_ISR_FE|USART_ISR_NE|USART_ISR_ORE);
 if(errors){
  uart_errors++;
  USART3->ICR=USART_ICR_PECF|USART_ICR_FECF|USART_ICR_NECF|USART_ICR_ORECF;
 }
}

void CDC_Bridge_DmaRxIrqHandler(void){
 const uint32_t flags=DMA1->ISR;
 if(flags&DMA_ISR_TEIF1){
  uart_errors++;
  USART3->CR3&=~USART_CR3_DMAR;
  DMA1_Channel1->CCR&=~DMA_CCR_EN;
  DMA1->IFCR=DMA_IFCR_CGIF1;
  uart_rx_restart_pending=1u;
  return;
 }
 if(flags&DMA_ISR_HTIF1)uart_rx_completed_halves++;
 if(flags&DMA_ISR_TCIF1)uart_rx_completed_halves++;
 DMA1->IFCR=DMA_IFCR_CGIF1;
}

void CDC_Bridge_DmaTxIrqHandler(void){
 const uint32_t flags=DMA1->ISR;
 if(!(flags&(DMA_ISR_TCIF2|DMA_ISR_TEIF2)))return;
 DMA1_Channel2->CCR&=~DMA_CCR_EN;
 DMA1->IFCR=DMA_IFCR_CGIF2;
 if(!uart_tx_dma_active)return;
 if(flags&DMA_ISR_TEIF2){
  uart_errors++;
  if(!uart_tx_dma_safety)uart_tx_dropped+=uart_tx_dma_length;
 }
 if(uart_tx_dma_safety){
  safety_active=0u;
 }else{
  uart_tx_tail=(uart_tx_tail+uart_tx_dma_length)%CDC_BRIDGE_BUF_SIZE;
  tx_at_line_boundary=(uart_tx_dma_last_byte=='\n'||uart_tx_dma_last_byte=='\r');
 }
 uart_tx_dma_active=0u;
 uart_tx_start();
}

/* Include pending HT/TC flags when the DMA has crossed a half-buffer edge
 * before its interrupt runs. The consumer is a monotonic byte offset. */
static uint32_t uart_rx_produced(void){
 const uint32_t irq_state=__get_PRIMASK();
 __disable_irq();
 const uint32_t halves=uart_rx_completed_halves;
 const uint32_t flags=DMA1->ISR;
 const uint32_t pending=((flags&DMA_ISR_HTIF1)?1U:0U)+
                        ((flags&DMA_ISR_TCIF1)?1U:0U);
 const uint32_t remaining=DMA1_Channel1->CNDTR;
 if(!irq_state)__enable_irq();
 __DMB();
 return BridgeDma_RxProduced(halves,pending,remaining);
}
static void reset_main(GPIO_PinState boot){SafetyHardware_PlanMainReset();HAL_GPIO_WritePin(BOOTSEL_MAIN_MCU_GPIO_Port,BOOTSEL_MAIN_MCU_Pin,boot);HAL_Delay(20);HAL_GPIO_WritePin(RESET_MAIN_MCU_GPIO_Port,RESET_MAIN_MCU_Pin,GPIO_PIN_RESET);HAL_Delay(50);HAL_GPIO_WritePin(RESET_MAIN_MCU_GPIO_Port,RESET_MAIN_MCU_Pin,GPIO_PIN_SET);bridge_app_wait_since=HAL_GetTick();bridge_reset_active=0;HAL_Delay(100);HAL_GPIO_WritePin(BOOTSEL_MAIN_MCU_GPIO_Port,BOOTSEL_MAIN_MCU_Pin,GPIO_PIN_RESET);}
static void send_control(const char*s){size_t n=strlen(s);if(n>APP_TX_DATA_SIZE)return;memcpy(UserTxBufferFS[1],s,n);CDC_Transmit_FS(1,UserTxBufferFS[1],(uint16_t)n);}
void CDC_Bridge_Process(void){
 bridge_out_arm_if_ready();
 const uint8_t action=requested_action;
 if(action){
  requested_action=0u;
  if(action==ACTION_BOOTLOADER){
   if(uart_set_bootloader_mode(1)){
    reset_main(GPIO_PIN_SET);
    uart_clear_bridge_buffers();
    send_control("BOOTLOADER OK\r\n");
   }else send_control("ERROR UART CONFIG\r\n");
  }else if(action==ACTION_APP||action==ACTION_RESET){
   if(uart_set_bootloader_mode(0)){
    reset_main(GPIO_PIN_RESET);
    send_control(action==ACTION_APP?"APP OK\r\n":"RESET OK\r\n");
   }else send_control("ERROR UART CONFIG\r\n");
  }else{
   const SafetyPolicy *safety=SafetyHardware_Status();
   char response[256];
   snprintf(response,sizeof(response),
    "STATUS BOOTSEL=%u RESET=%u UART_MODE=%s UART_BAUD=%lu APP_WAIT=%u "
    "UART_ERRORS=%lu RX_DROPPED=%lu TX_DROPPED=%lu "
    "SAFETY_STATE=%u SAFETY_FAULTS=%08lX "
    "SAFETY_TRIP=%u SAFETY_TRIP_INPUTS=%02X PWR2_EN=%u\r\n",
    HAL_GPIO_ReadPin(BOOTSEL_MAIN_MCU_GPIO_Port,BOOTSEL_MAIN_MCU_Pin),
    HAL_GPIO_ReadPin(RESET_MAIN_MCU_GPIO_Port,RESET_MAIN_MCU_Pin),
    uart_bootloader_mode?"BOOT_8E1":"APP_8N1",
    (unsigned long)(uart_bootloader_mode?460800U:921600U),bridge_waiting_for_app,
    (unsigned long)uart_errors,(unsigned long)uart_rx_dropped,
    (unsigned long)uart_tx_dropped,(unsigned)safety->state,
    (unsigned long)safety->faults,
    (unsigned)safety->trip_reason, (unsigned)safety->trip_inputs,
    SafetyPolicy_PowerEnabled(safety) ? 1u : 0u);
   send_control(response);
  }
 }
 if(bridge_waiting_for_app&&!bridge_reset_active&&
    (uint32_t)(HAL_GetTick()-bridge_app_wait_since)>=1500U){
  bridge_waiting_for_app=0u;
  uart_tx_start();
 }
 if(bridge_paused)return;
 /* The safety pins are polled on every main-loop pass. RX DMA has enough
  * headroom for a millisecond of UART traffic, so keep bridge bookkeeping
  * off the hot safety path until the next USB frame. */
 const uint32_t poll_ms=HAL_GetTick();
 if(!uart_rx_restart_pending&&poll_ms==bridge_rx_last_poll_ms)return;
 bridge_rx_last_poll_ms=poll_ms;
 if(uart_rx_restart_pending){
  const uint32_t irq_state=__get_PRIMASK();
  __disable_irq();
  USART3->CR3&=~USART_CR3_DMAR;
  DMA1_Channel1->CCR&=~DMA_CCR_EN;
  DMA1->IFCR=DMA_IFCR_CGIF1;
  uart_rx_completed_halves=0u;
  uart_rx_consumed=0u;
  uart_rx_scanned=0u;
  DMA1_Channel1->CNDTR=BRIDGE_RX_DMA_SIZE;
  uart_rx_restart_pending=0u;
  DMA1_Channel1->CCR|=DMA_CCR_EN;
  USART3->CR3|=USART_CR3_DMAR;
  if(!irq_state)__enable_irq();
 }
 const uint32_t produced=uart_rx_produced();
 if(bridge_waiting_for_app&&!bridge_reset_active){
  uint32_t scan=uart_rx_scanned;
  if(produced-scan>BRIDGE_RX_DMA_SIZE)scan=produced-BRIDGE_RX_DMA_SIZE;
  while(scan!=produced){
   if(uart_rx_dma[scan&(BRIDGE_RX_DMA_SIZE-1U)]==0u){
    bridge_waiting_for_app=0u;
    uart_tx_start();
    break;
   }
   scan++;
  }
  uart_rx_scanned=produced;
 }
 const uint32_t safe_capacity=BRIDGE_RX_DMA_SIZE-APP_TX_DATA_SIZE;
 if(produced-uart_rx_consumed>safe_capacity){
  uart_rx_dropped+=produced-uart_rx_consumed-safe_capacity;
  uart_rx_consumed=produced-safe_capacity;
 }
 if(!usb_tx_busy[0]&&produced!=uart_rx_consumed){
  const uint16_t available=(uint16_t)(produced-uart_rx_consumed);
  const uint16_t count=available<APP_TX_DATA_SIZE?available:APP_TX_DATA_SIZE;
  const uint16_t start=uart_rx_consumed&(BRIDGE_RX_DMA_SIZE-1U);
  const uint16_t first=count<BRIDGE_RX_DMA_SIZE-start?count:BRIDGE_RX_DMA_SIZE-start;
  memcpy(UserTxBufferFS[0],&uart_rx_dma[start],first);
  memcpy(UserTxBufferFS[0]+first,uart_rx_dma,count-first);
  if(CDC_Transmit_FS(0,UserTxBufferFS[0],count)==USBD_OK)
   uart_rx_consumed+=count;
 }
}
uint8_t CDC_IsBridgeMode(void){return 1;}void CDC_DebugReportStartup(void){if(uart_set_bootloader_mode(0))reset_main(GPIO_PIN_RESET);}
