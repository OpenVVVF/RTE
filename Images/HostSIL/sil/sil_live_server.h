/*
 * sil_live_server.h — live telemetry link for host_sil.
 *
 * A small non-blocking TCP server that proxies the firmware's own USART3 TX
 * byte stream (COBS-framed InverterProtocol packets produced by the firmware
 * Telemetry module, tapped in sil_hal.cpp's HAL_UART_Transmit_DMA) verbatim
 * to every connected client.  RTEStudio attaches to this stream exactly like
 * it does to HostSim:
 *
 *   RTEStudio --tcp 127.0.0.1:14608 --protocol ivp
 *
 * The bytes are not re-framed or re-sampled: whatever the firmware puts on
 * its UART TX DMA goes onto the socket unmodified, at the firmware's own
 * cadence (100 Hz DATA + periodic DEFINE re-announce, so a client connecting
 * at any point has the full key table within ~100 ms).
 *
 * Threading: sil_live_feed_tx() runs on the firmware context (from the HAL
 * tap); sil_live_poll()/sil_live_stop() run on the scheduler context.  The
 * cooperative runtime keeps the two contexts strictly alternating, and the
 * pending buffer is additionally mutex-guarded.
 * RX direction: bytes a client sends are fed verbatim into the modeled
 * huart3 IT-RX path (silUartRxEnqueue -> silUartRxPoll -> the firmware's
 * HAL_UART_RxCpltCallback), so the Gen6FW CommandShell sees them exactly as
 * minicom-typed bytes on hardware; text lines are additionally logged to
 * stdout for observability.
 */
#ifndef SIL_LIVE_SERVER_H
#define SIL_LIVE_SERVER_H

#include <cstddef>
#include <cstdint>

/* Default listen port (matches HostSim's live port). */
#define SIL_LIVE_DEFAULT_PORT 14608

/* Open the listen socket (bound to host:port, non-blocking).
 * Returns false (and logs why) if the socket could not be created/bound —
 * the simulation continues without a live link in that case. */
bool sil_live_start(const char* host, uint16_t port);

/* Close listen socket and all clients. */
void sil_live_stop();

/* True while the server is listening. */
bool sil_live_active();

/* Firmware context: queue UART TX bytes for all connected clients.
 * No-op when the server is not running; zero copies when there is neither a
 * client nor pending data. */
void sil_live_feed_tx(const uint8_t* data, size_t len);

/* Scheduler context: accept pending connects, flush queued bytes to every
 * client (slow clients are dropped), and drain client RX bytes into the
 * modeled huart3 IT-RX FIFO (silUartRxEnqueue).
 * Call once per app-loop iteration. */
void sil_live_poll();

#endif /* SIL_LIVE_SERVER_H */
