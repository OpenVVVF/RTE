/*
 * sil_fw_console.h — host-side mirror of the firmware's "print" telemetry
 * strings onto stdout.
 *
 * In --live mode the firmware's USART3 byte stream is already observable via
 * RTEStudio / ivp_probe.py on the TCP link; in batch mode the bytes are
 * dropped.  This module taps the same stream (fed from
 * HAL_UART_Transmit_DMA), decodes the COBS-framed InverterProtocol packets
 * with the shared Lib/InverterProtocol walker, and prints complete "print"
 * key strings as "[FW ...]" lines — so boot messages, fault raises
 * ([FAULT][C][...] Name triggered: reason), and supervisor transitions are
 * visible in batch logs with the firmware's own timestamps.
 *
 * Call from the scheduler context only (the UART shim runs there / at ISR
 * points while the firmware is blocked).
 */
#ifndef SIL_FW_CONSOLE_H
#define SIL_FW_CONSOLE_H

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/* Clear all decoder state (call once before the firmware thread starts). */
void silFwConsoleReset();

/* Feed one chunk of the firmware's USART3 TX byte stream. */
void silFwConsoleFeed(const uint8_t* data, size_t len);

/* Enable/disable the mirror (default: enabled). */
void silFwConsoleSetEnabled(bool enabled);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SIL_FW_CONSOLE_H */
