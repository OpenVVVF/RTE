/*
 * sil_uart_cpp.cpp — C++-linkage spellings of the MCP2221A console API.
 *
 * mcp2221a_driver.h lacks extern "C" guards: firmware C++ TUs therefore
 * reference the mangled C++ names, while the implementations compiled as C
 * (sil_uart_c.c) export the plain names.  These wrappers bridge the two.
 */
#include "mcp2221a_driver.h"

#include <cstdarg>
#include <cstdio>

extern "C" {
void sil_mcp_impl_init(UART_HandleTypeDef* huart);
void sil_mcp_impl_transmit(const uint8_t* data, uint16_t len);
void sil_mcp_impl_print(const char* str);
void sil_mcp_impl_println(const char* str);
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
