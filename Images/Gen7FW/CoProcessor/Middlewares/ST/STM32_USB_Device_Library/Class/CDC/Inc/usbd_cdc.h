#ifndef __USB_CDC_H
#define __USB_CDC_H
#include "usbd_ioreq.h"

#define CDC_PORT_BRIDGE 0U
#define CDC_PORT_CONTROL 1U
#define CDC_PORT_COUNT 2U
#define CDC0_IN_EP 0x81U
#define CDC0_OUT_EP 0x01U
#define CDC0_CMD_EP 0x82U
#define CDC1_IN_EP 0x83U
#define CDC1_OUT_EP 0x03U
#define CDC1_CMD_EP 0x84U
#define CDC_DATA_FS_MAX_PACKET_SIZE 64U
#define CDC_CMD_PACKET_SIZE 8U
#define USB_CDC_CONFIG_DESC_SIZ 141U
#define CDC_REQ_MAX_DATA_SIZE 7U
#define CDC_FS_BINTERVAL 0x10U
#define CDC_SET_LINE_CODING 0x20U
#define CDC_GET_LINE_CODING 0x21U
#define CDC_SET_CONTROL_LINE_STATE 0x22U

typedef struct {
  int8_t (*Init)(uint8_t port);
  int8_t (*DeInit)(uint8_t port);
  int8_t (*Control)(uint8_t port, uint8_t cmd, uint8_t *buf, uint16_t length);
  int8_t (*Receive)(uint8_t port, uint8_t *buf, uint32_t *length);
  int8_t (*TransmitCplt)(uint8_t port, uint8_t *buf, uint32_t *length, uint8_t epnum);
} USBD_CDC_ItfTypeDef;

typedef struct {
  uint32_t data[2];
  uint8_t cmd_opcode, cmd_length, cmd_port;
  uint8_t *rx_buffer[CDC_PORT_COUNT], *tx_buffer[CDC_PORT_COUNT];
  uint32_t rx_length[CDC_PORT_COUNT], tx_length[CDC_PORT_COUNT];
  __IO uint32_t tx_state[CDC_PORT_COUNT];
} USBD_CDC_HandleTypeDef;

extern USBD_ClassTypeDef USBD_CDC;
uint8_t USBD_CDC_RegisterInterface(USBD_HandleTypeDef *, USBD_CDC_ItfTypeDef *);
uint8_t USBD_CDC_SetTxBuffer(USBD_HandleTypeDef *, uint8_t, uint8_t *, uint32_t);
uint8_t USBD_CDC_SetRxBuffer(USBD_HandleTypeDef *, uint8_t, uint8_t *);
uint8_t USBD_CDC_ReceivePacket(USBD_HandleTypeDef *, uint8_t);
uint8_t USBD_CDC_TransmitPacket(USBD_HandleTypeDef *, uint8_t);
#endif
