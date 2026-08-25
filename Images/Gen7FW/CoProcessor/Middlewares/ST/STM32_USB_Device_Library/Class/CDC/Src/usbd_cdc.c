#include <string.h>
#include "usbd_cdc.h"
#include "usbd_ctlreq.h"

static uint8_t init(USBD_HandleTypeDef *, uint8_t), deinit(USBD_HandleTypeDef *, uint8_t);
static uint8_t setup(USBD_HandleTypeDef *, USBD_SetupReqTypedef *), ep0_rx(USBD_HandleTypeDef *);
static uint8_t data_in(USBD_HandleTypeDef *, uint8_t), data_out(USBD_HandleTypeDef *, uint8_t);
static uint8_t *cfg(uint16_t *), *other_cfg(uint16_t *), *qualifier(uint16_t *);

USBD_ClassTypeDef USBD_CDC = { init, deinit, setup, NULL, ep0_rx, data_in, data_out,
  NULL, NULL, NULL, cfg, cfg, other_cfg, qualifier };

#define WB(x) LOBYTE(x), HIBYTE(x)
#define FUNCTION(ci,di,ce,oe,ie,si) \
  8,0x0B,ci,2,2,2,0,0, \
  9,USB_DESC_TYPE_INTERFACE,ci,0,1,2,2,0,si, \
  5,0x24,0,0x10,1, 5,0x24,1,0,di, 4,0x24,2,2, 5,0x24,6,ci,di, \
  7,USB_DESC_TYPE_ENDPOINT,ce,3,WB(CDC_CMD_PACKET_SIZE),CDC_FS_BINTERVAL, \
  9,USB_DESC_TYPE_INTERFACE,di,0,2,0x0A,0,0,si, \
  7,USB_DESC_TYPE_ENDPOINT,oe,2,WB(CDC_DATA_FS_MAX_PACKET_SIZE),0, \
  7,USB_DESC_TYPE_ENDPOINT,ie,2,WB(CDC_DATA_FS_MAX_PACKET_SIZE),0

__ALIGN_BEGIN static uint8_t descriptor[USB_CDC_CONFIG_DESC_SIZ] __ALIGN_END = {
  9,USB_DESC_TYPE_CONFIGURATION,WB(USB_CDC_CONFIG_DESC_SIZ),4,1,0,
#if (USBD_SELF_POWERED == 1U)
  0xC0,
#else
  0x80,
#endif
  USBD_MAX_POWER,
  FUNCTION(0,1,CDC0_CMD_EP,CDC0_OUT_EP,CDC0_IN_EP,5),
  FUNCTION(2,3,CDC1_CMD_EP,CDC1_OUT_EP,CDC1_IN_EP,6)
};
__ALIGN_BEGIN static uint8_t qualifier_desc[USB_LEN_DEV_QUALIFIER_DESC] __ALIGN_END =
  { USB_LEN_DEV_QUALIFIER_DESC,USB_DESC_TYPE_DEVICE_QUALIFIER,0,2,0xEF,2,1,64,1,0 };
static const uint8_t in_ep[] = {CDC0_IN_EP,CDC1_IN_EP};
static const uint8_t out_ep[] = {CDC0_OUT_EP,CDC1_OUT_EP};
static const uint8_t cmd_ep[] = {CDC0_CMD_EP,CDC1_CMD_EP};

static uint8_t init(USBD_HandleTypeDef *d, uint8_t n) {
  UNUSED(n); USBD_CDC_HandleTypeDef *h=USBD_malloc(sizeof(*h));
  if (!h) return USBD_EMEM;
  memset(h,0,sizeof(*h)); h->cmd_opcode=0xFF; d->pClassData=h;
  for(uint8_t p=0;p<2;p++) {
    USBD_LL_OpenEP(d,in_ep[p],USBD_EP_TYPE_BULK,64); USBD_LL_OpenEP(d,out_ep[p],USBD_EP_TYPE_BULK,64);
    USBD_LL_OpenEP(d,cmd_ep[p],USBD_EP_TYPE_INTR,8);
    d->ep_in[in_ep[p]&15].is_used=1; d->ep_out[out_ep[p]&15].is_used=1; d->ep_in[cmd_ep[p]&15].is_used=1;
    d->ep_in[cmd_ep[p]&15].bInterval=CDC_FS_BINTERVAL;
    ((USBD_CDC_ItfTypeDef*)d->pUserData)->Init(p);
    USBD_LL_PrepareReceive(d,out_ep[p],h->rx_buffer[p],64);
  } return USBD_OK;
}
static uint8_t deinit(USBD_HandleTypeDef *d, uint8_t n) {
  UNUSED(n); for(uint8_t p=0;p<2;p++) { USBD_LL_CloseEP(d,in_ep[p]); USBD_LL_CloseEP(d,out_ep[p]);
    USBD_LL_CloseEP(d,cmd_ep[p]); ((USBD_CDC_ItfTypeDef*)d->pUserData)->DeInit(p); }
  if(d->pClassData) USBD_free(d->pClassData);
  d->pClassData=NULL; return USBD_OK;
}
static uint8_t setup(USBD_HandleTypeDef *d, USBD_SetupReqTypedef *r) {
  USBD_CDC_HandleTypeDef *h=d->pClassData; uint8_t alt=0,p=(r->wIndex>=2)?1:0; uint16_t st=0;
  if(!h) return USBD_FAIL;
  if((r->bmRequest&USB_REQ_TYPE_MASK)==USB_REQ_TYPE_CLASS) {
    if(r->wLength) { if(r->bmRequest&0x80) { ((USBD_CDC_ItfTypeDef*)d->pUserData)->Control(p,r->bRequest,(uint8_t*)h->data,r->wLength);
        USBD_CtlSendData(d,(uint8_t*)h->data,MIN(CDC_REQ_MAX_DATA_SIZE,r->wLength)); }
      else { h->cmd_opcode=r->bRequest; h->cmd_length=(uint8_t)r->wLength; h->cmd_port=p;
        USBD_CtlPrepareRx(d,(uint8_t*)h->data,r->wLength); } }
    else ((USBD_CDC_ItfTypeDef*)d->pUserData)->Control(p,r->bRequest,(uint8_t*)r,0);
    return USBD_OK;
  }
  if((r->bmRequest&USB_REQ_TYPE_MASK)==USB_REQ_TYPE_STANDARD) {
    if(r->bRequest==USB_REQ_GET_STATUS) USBD_CtlSendData(d,(uint8_t*)&st,2);
    else if(r->bRequest==USB_REQ_GET_INTERFACE) USBD_CtlSendData(d,&alt,1);
    else if(r->bRequest!=USB_REQ_SET_INTERFACE && r->bRequest!=USB_REQ_CLEAR_FEATURE) {USBD_CtlError(d,r);return USBD_FAIL;}
    return USBD_OK;
  } USBD_CtlError(d,r); return USBD_FAIL;
}
static uint8_t ep0_rx(USBD_HandleTypeDef *d) { USBD_CDC_HandleTypeDef *h=d->pClassData;
  if(h && h->cmd_opcode!=0xFF) { ((USBD_CDC_ItfTypeDef*)d->pUserData)->Control(h->cmd_port,h->cmd_opcode,(uint8_t*)h->data,h->cmd_length); h->cmd_opcode=0xFF; } return USBD_OK; }
static int8_t in_port(uint8_t e) { return e==(CDC0_IN_EP&15)?0:e==(CDC1_IN_EP&15)?1:-1; }
static uint8_t data_in(USBD_HandleTypeDef *d,uint8_t e) { USBD_CDC_HandleTypeDef *h=d->pClassData; int8_t p=in_port(e); PCD_HandleTypeDef *pcd=d->pData;
  if(!h||p<0)return USBD_FAIL;
  if(d->ep_in[e].total_length && !(d->ep_in[e].total_length%pcd->IN_ep[e].maxpacket)) {d->ep_in[e].total_length=0;USBD_LL_Transmit(d,e,NULL,0);}
  else {h->tx_state[p]=0;if(((USBD_CDC_ItfTypeDef*)d->pUserData)->TransmitCplt)((USBD_CDC_ItfTypeDef*)d->pUserData)->TransmitCplt(p,h->tx_buffer[p],&h->tx_length[p],e);} return USBD_OK; }
static uint8_t data_out(USBD_HandleTypeDef *d,uint8_t e) { USBD_CDC_HandleTypeDef *h=d->pClassData; int8_t p=e==(CDC0_OUT_EP&15)?0:e==(CDC1_OUT_EP&15)?1:-1;
  if(!h||p<0)return USBD_FAIL;
  h->rx_length[p]=USBD_LL_GetRxDataSize(d,e); ((USBD_CDC_ItfTypeDef*)d->pUserData)->Receive(p,h->rx_buffer[p],&h->rx_length[p]); return USBD_OK; }
static uint8_t *cfg(uint16_t*l){descriptor[1]=USB_DESC_TYPE_CONFIGURATION;*l=sizeof(descriptor);return descriptor;}
static uint8_t *other_cfg(uint16_t*l){descriptor[1]=USB_DESC_TYPE_OTHER_SPEED_CONFIGURATION;*l=sizeof(descriptor);return descriptor;}
static uint8_t *qualifier(uint16_t*l){*l=sizeof(qualifier_desc);return qualifier_desc;}
uint8_t USBD_CDC_RegisterInterface(USBD_HandleTypeDef*d,USBD_CDC_ItfTypeDef*f){if(!f)return USBD_FAIL;d->pUserData=f;return USBD_OK;}
uint8_t USBD_CDC_SetTxBuffer(USBD_HandleTypeDef*d,uint8_t p,uint8_t*b,uint32_t l){USBD_CDC_HandleTypeDef*h=d->pClassData;if(!h||p>=2)return USBD_FAIL;h->tx_buffer[p]=b;h->tx_length[p]=l;return USBD_OK;}
uint8_t USBD_CDC_SetRxBuffer(USBD_HandleTypeDef*d,uint8_t p,uint8_t*b){USBD_CDC_HandleTypeDef*h=d->pClassData;if(!h||p>=2)return USBD_FAIL;h->rx_buffer[p]=b;return USBD_OK;}
uint8_t USBD_CDC_ReceivePacket(USBD_HandleTypeDef*d,uint8_t p){USBD_CDC_HandleTypeDef*h=d->pClassData;if(!h||p>=2)return USBD_FAIL;USBD_LL_PrepareReceive(d,out_ep[p],h->rx_buffer[p],64);return USBD_OK;}
uint8_t USBD_CDC_TransmitPacket(USBD_HandleTypeDef*d,uint8_t p){USBD_CDC_HandleTypeDef*h=d->pClassData;if(!h||p>=2)return USBD_FAIL;if(h->tx_state[p])return USBD_BUSY;h->tx_state[p]=1;d->ep_in[in_ep[p]&15].total_length=h->tx_length[p];USBD_LL_Transmit(d,in_ep[p],h->tx_buffer[p],h->tx_length[p]);return USBD_OK;}
