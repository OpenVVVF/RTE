#pragma once

#include <cstdint>
#include <cstddef>

namespace Inverter {

/**
 * @brief Lightweight plain-text command shell on the telemetry UART.
 *
 * Receives one byte at a time via HAL_UART_Receive_IT and parses complete
 * lines in the main loop.  All echo traffic uses Telemetry::printf so it
 * appears in the ImGui console without corrupting binary telemetry frames.
 */
class CommandShell {
public:
    CommandShell() = default;

    /**
     * @brief Start receiving on huart3.
     */
    bool init();

    /**
     * @brief Parse any received lines and dispatch commands.
     */
    void poll();

    /**
     * @brief Called from HAL_UART_RxCpltCallback: read the byte and restart RX.
     */
    void onRxComplete();

    /**
     * @brief Recover from a UART error (overrun/noise/frame) and restart RX.
     */
    void recover();

    uint32_t uartErrorCount() const { return m_uart_errors; }
    uint32_t rxDroppedCount() const { return m_rx_dropped; }
    uint32_t rxRearmFailureCount() const { return m_rx_rearm_failures; }

private:
    static constexpr size_t RX_BUF_SIZE = 256;
    static constexpr size_t LINE_SIZE   = 128;

    uint8_t m_rx_buf[RX_BUF_SIZE]{};
    volatile size_t m_rx_head = 0;
    volatile size_t m_rx_tail = 0;
    volatile bool m_rx_corrupt = false;

    uint8_t m_hal_rx_byte = 0;      // separate byte for HAL_UART_Receive_IT

    char m_line[LINE_SIZE]{};
    size_t m_line_len = 0;
    bool m_discard_line = false;

    bool m_initialized = false;
    volatile uint32_t m_uart_errors = 0;
    volatile uint32_t m_rx_dropped = 0;
    volatile uint32_t m_rx_rearm_failures = 0;
};

/**
 * @brief Global shell instance used by the UART callback.
 */
CommandShell& commandShell();

} // namespace Inverter
