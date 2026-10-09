#include "main.h"
#include <assert.h>
#include <string.h>

/* Use the real USB callbacks, DMA completion and bridge application loop. */
#include "../usbd_cdc_if.c"

TestUsart test_usart;
TestDmaChannel test_rx_channel, test_tx_channel;
TestDma test_dma;
TestMux test_rx_mux, test_tx_mux;
UART_HandleTypeDef huart3;
USBD_HandleTypeDef hUsbDeviceFS;
static uint32_t now_ms, produced_bytes, reset_plans;
static bool usb_configured;
static SafetyPolicy policy;
static uint8_t transmitted[4096];
static size_t transmitted_length;

uint32_t HAL_GetTick(void) { return now_ms; }
void HAL_Delay(uint32_t ms) { now_ms += ms; }
void HAL_GPIO_WritePin(unsigned p, unsigned n, GPIO_PinState s) { (void)p; (void)n; (void)s; }
GPIO_PinState HAL_GPIO_ReadPin(unsigned p, unsigned n) { (void)p; (void)n; return GPIO_PIN_SET; }
const SafetyPolicy *SafetyHardware_Status(void) { return &policy; }
void SafetyHardware_PlanMainReset(void) { ++reset_plans; }
void SafetyHardware_MainResetAsserted(void) {}
uint8_t USBD_CDC_SetTxBuffer(USBD_HandleTypeDef *d, uint8_t p, uint8_t *b, uint32_t n) {
    (void)d; (void)p; (void)b; (void)n; return usb_configured ? USBD_OK : USBD_FAIL;
}
uint8_t USBD_CDC_SetRxBuffer(USBD_HandleTypeDef *d, uint8_t p, uint8_t *b) {
    (void)d; (void)p; (void)b; return USBD_OK;
}
uint8_t USBD_CDC_TransmitPacket(USBD_HandleTypeDef *d, uint8_t p) { (void)d; (void)p; return USBD_OK; }
uint8_t USBD_CDC_ReceivePacket(USBD_HandleTypeDef *d, uint8_t p) { (void)d; (void)p; return USBD_OK; }

static void usb_command(const char *command) {
    uint32_t length = (uint32_t)strlen(command);
    cdc_receive(0u, (uint8_t *)command, &length);
}
static void complete_tx(void) {
    assert(uart_tx_dma_active);
    const uint8_t *source = uart_tx_dma_safety
        ? (const uint8_t *)safety_active_frame : uart_tx_ring + uart_tx_tail;
    assert(transmitted_length + uart_tx_dma_length <= sizeof(transmitted));
    memcpy(transmitted + transmitted_length, source, uart_tx_dma_length);
    transmitted_length += uart_tx_dma_length;
    DMA1->ISR = DMA_ISR_TCIF2;
    CDC_Bridge_DmaTxIrqHandler();
    DMA1->ISR = 0u;
}
static void receive_uart(const uint8_t *bytes, size_t count) {
    for (size_t i = 0; i < count; ++i)
        uart_rx_dma[(produced_bytes++) & (BRIDGE_RX_DMA_SIZE - 1u)] = bytes[i];
    uart_rx_completed_halves = produced_bytes / BRIDGE_RX_DMA_HALF;
    DMA1_Channel1->CNDTR = BRIDGE_RX_DMA_SIZE - (produced_bytes % BRIDGE_RX_DMA_SIZE);
}
static void telemetry(bool corrupt) {
    uint8_t raw[IVP_HEADER_SIZE + 3u] = {0}, encoded[64];
    ivp_write_u32le(raw, IVP_MAGIC);
    raw[4] = IVP_VERSION;
    raw[5] = IVP_MSG_TELEMETRY_DEFINE;
    ivp_write_u16le(raw + 6u, 1u);
    ivp_write_u16le(raw + sizeof(raw) - 2u, ivp_crc16_ccitt(raw, sizeof(raw) - 2u));
    if (corrupt) raw[8] ^= 1u;
    size_t length = ivp_cobs_encode(raw, sizeof(raw), encoded, sizeof(encoded));
    encoded[length++] = 0u;
    receive_uart(encoded, length);
}
static void poll(void) { ++now_ms; CDC_Bridge_Process(); }
static void reset_bridge(void) {
    assert(uart_set_bootloader_mode(0u));
    produced_bytes = 0u;
    transmitted_length = 0u;
    bridge_reset_active = 0u;
    bridge_begin_app_wait(false);
}
int main(void) {
    const SafetyFaultFrame status = {0u, 1u, 2u, SAFETY_CLEAR_NONE};
    reset_bridge();
    usb_command("\x18\nprobe\n");
    CDC_Bridge_QueueSafetyStatus(&status);
    now_ms += 3000u;
    poll();
    assert(bridge_waiting_for_app && !uart_tx_dma_active);
    const uint8_t noise[] = {0u, 1u, 0u};
    receive_uart(noise, sizeof(noise));
    telemetry(true);
    poll();
    assert(bridge_waiting_for_app && !uart_tx_dma_active);
    telemetry(false);
    poll();
    assert(!bridge_waiting_for_app && uart_tx_dma_safety);
    complete_tx(); /* Private status precedes the first host command. */
    while (uart_tx_dma_active) complete_tx();
    assert(transmitted_length == SAFETY_FAULT_WIRE_LENGTH + 8u);
    assert(memcmp(transmitted + SAFETY_FAULT_WIRE_LENGTH, "\x18\nprobe\n", 8u) == 0);

    /* An incomplete command never reaches the H7 or blocks safety reports. */
    usb_command("rebo");
    assert(!uart_tx_dma_active && tx_at_line_boundary);
    CDC_Bridge_QueueSafetyStatus(&status);
    assert(uart_tx_dma_safety);
    complete_tx();
    assert(!uart_tx_dma_active && tx_at_line_boundary);
    usb_command("ot\nprobe\n");
    assert(bridge_reboot_pending && !uart_tx_dma_active);
    poll();
    assert(reset_plans == 1u && uart_tx_dma_reboot);
    complete_tx();
    assert(bridge_waiting_for_app && !uart_tx_dma_active);
    telemetry(false); /* Old telemetry immediately after the reboot line. */
    poll();
    assert(bridge_waiting_for_app && !uart_tx_dma_active);
    now_ms += BRIDGE_APP_QUIET_MS;
    telemetry(false);
    poll();
    assert(!bridge_waiting_for_app && uart_tx_dma_active);
    complete_tx();

    /* USB BUSY and unconfigured states only discard old telemetry. Safety
     * output continues with bounded RX backlog; no Studio reader is needed. */
    uint8_t data[1000];
    memset(data, 0xFF, sizeof(data));
    for (unsigned mode = 0u; mode < 2u; ++mode) {
        usb_configured = mode != 0u;
        usb_tx_busy[0] = mode != 0u;
        const uint32_t before = uart_rx_dropped;
        for (unsigned i = 0u; i < 30u; ++i) {
            receive_uart(data, sizeof(data));
            poll();
            assert(produced_bytes - uart_rx_consumed <= BRIDGE_RX_DMA_SIZE - APP_TX_DATA_SIZE);
        }
        assert(uart_rx_dropped > before);
        CDC_Bridge_QueueSafetyStatus(&status);
        assert(uart_tx_dma_safety);
        complete_tx();
    }

    /* ROM bootloader bytes bypass all shell staging/readiness handling. */
    assert(uart_set_bootloader_mode(1u));
    usb_command("\x7f");
    assert(uart_tx_dma_active && uart_tx_dma_length == 1u);
    complete_tx();
    return 0;
}
