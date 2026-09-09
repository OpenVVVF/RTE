/*
 * sil_uart_c.c — SIL replacement for Src/Inverter/Drivers/UART/mcp2221a_driver.c,
 * C-linkage half (firmware C TUs reference these unmangled names).
 *
 * The MCP2221A text console maps to the host stdout; the USB-enumeration
 * delay is skipped (not load-bearing).  Binary telemetry frames do not use
 * this path (Telemetry transmits via HAL_UART_Transmit_DMA directly, which
 * the SIL HAL swallows).
 *
 * mcp2221a_driver.h has no extern "C" guards, so firmware C++ TUs reference
 * the mangled C++ spellings — those live in sil_uart_cpp.cpp and forward to
 * the sil_mcp_impl_* aliases here.
 */
#include "mcp2221a_driver.h"

#include <stdio.h>

void sil_mcp_impl_init(UART_HandleTypeDef* huart) {
    (void)huart;
}

void sil_mcp_impl_transmit(const uint8_t* data, uint16_t len) {
    if (data == NULL) return;
    (void)fwrite(data, 1, len, stdout);
    fflush(stdout);
}

void sil_mcp_impl_print(const char* str) {
    if (str == NULL) return;
    fputs(str, stdout);
    fflush(stdout);
}

void sil_mcp_impl_println(const char* str) {
    if (str == NULL) return;
    fputs(str, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void MCP2221A_Init(UART_HandleTypeDef* huart) {
    sil_mcp_impl_init(huart);
}

void MCP2221A_Transmit(const uint8_t* data, uint16_t len) {
    sil_mcp_impl_transmit(data, len);
}

void MCP2221A_Print(const char* str) {
    sil_mcp_impl_print(str);
}

void MCP2221A_PrintLn(const char* str) {
    sil_mcp_impl_println(str);
}

void MCP2221A_Printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

int __io_putchar(int ch) {
    return putchar(ch);
}
