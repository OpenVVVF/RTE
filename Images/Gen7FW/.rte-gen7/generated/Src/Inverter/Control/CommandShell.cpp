#include "Inverter/Control/CommandShell.h"
#include "Inverter/Command/CommandManager.h"
#include "Inverter/Command/CommandInitializer.h"
#include "Inverter/Command/CommandContext.h"
#include "Inverter/Telemetry.h"

#include "main.h"
#include "usart.h"

#include <cctype>
#include <cstring>

namespace Inverter {

static CommandShell s_instance;
static CommandContext s_commandContext;

CommandShell& commandShell() {
    return s_instance;
}

bool CommandShell::init() {
    if (m_initialized) {
        return true;
    }

    m_rx_head = 0;
    m_rx_tail = 0;
    m_rx_corrupt = false;
    m_line_len = 0;
    m_discard_line = false;
    m_initialized = true;

    /* USART3 was initialized before the storage and sensor startup delays.
     * Drop any byte left in RDR while the shell was not receiving; otherwise
     * it prefixes the first command and makes that one line unrecognizable. */
    __HAL_UART_SEND_REQ(&huart3, UART_RXDATA_FLUSH_REQUEST);

    /* Clear any stale error/idle flags left from the power-up / debugger
     * transient before unmasking the UART interrupt. */
    __HAL_UART_CLEAR_FLAG(&huart3, UART_CLEAR_PEF | UART_CLEAR_FEF |
                                  UART_CLEAR_NEF | UART_CLEAR_OREF |
                                  UART_CLEAR_IDLEF);
    HAL_NVIC_ClearPendingIRQ(USART3_IRQn);

    /* Register before arming RX so poll() can retry a failed first arm. */
    initializeCommands();
    CommandManager::instance().setContext(s_commandContext);

    HAL_StatusTypeDef status = HAL_UART_Receive_IT(&huart3, &m_hal_rx_byte, 1U);
    if (status != HAL_OK) {
        ++m_rx_rearm_failures;
        Telemetry::printf("[SHELL] ERROR: HAL_UART_Receive_IT failed");
        return false;
    }

    Telemetry::printf("[SHELL] Command shell ready; type HELP for list");
    return true;
}

void CommandShell::onRxComplete() {
    /* Read the byte from HAL's private scratch location, not from the ring
     * buffer, so incoming bytes cannot overwrite data the main loop has not
     * yet consumed. */
    uint8_t b = m_hal_rx_byte;

    size_t next = (m_rx_head + 1U) % RX_BUF_SIZE;
    if (next != m_rx_tail) {
        m_rx_buf[m_rx_head] = b;
        m_rx_head = next;
    } else {
        /* The queued bytes can contain a truncated command. Drop the whole
         * queue and ignore input until the next line delimiter. */
        m_rx_dropped += RX_BUF_SIZE;
        m_rx_tail = m_rx_head;
        m_rx_corrupt = true;
    }

    /* Restart reception immediately. */
    if (HAL_UART_Receive_IT(&huart3, &m_hal_rx_byte, 1U) != HAL_OK) {
        ++m_rx_rearm_failures;
    }
}

void CommandShell::recover() {
    if (!m_initialized) return;
    ++m_uart_errors;
    m_rx_tail = m_rx_head;
    m_rx_corrupt = true;

    /* Clear error/idle flags and restart reception. */
    __HAL_UART_CLEAR_FLAG(&huart3, UART_CLEAR_PEF | UART_CLEAR_FEF |
                                  UART_CLEAR_NEF | UART_CLEAR_OREF |
                                  UART_CLEAR_IDLEF);
    HAL_NVIC_ClearPendingIRQ(USART3_IRQn);
    if (HAL_UART_Receive_IT(&huart3, &m_hal_rx_byte, 1U) != HAL_OK) {
        ++m_rx_rearm_failures;
    }
}

void CommandShell::poll() {
    if (!m_initialized) {
        return;
    }

    /* An IRQ callback can fail to rearm RX during an error transition. A
     * ready HAL receive state means there is no active one-byte receive. */
    if (huart3.RxState == HAL_UART_STATE_READY &&
        HAL_UART_Receive_IT(&huart3, &m_hal_rx_byte, 1U) != HAL_OK) {
        ++m_rx_rearm_failures;
    }

    while (true) {
        __disable_irq();
        bool corrupt = m_rx_corrupt;
        m_rx_corrupt = false;
        bool empty = (m_rx_head == m_rx_tail);
        uint8_t b = empty ? 0U : m_rx_buf[m_rx_tail];
        if (!empty) {
            m_rx_tail = (m_rx_tail + 1U) % RX_BUF_SIZE;
        }
        __enable_irq();

        if (corrupt) {
            m_line_len = 0;
            m_line[0] = '\0';
            m_discard_line = true;
        }

        if (empty) {
            break;
        }

        if (m_discard_line) {
            if (b == '\r' || b == '\n') m_discard_line = false;
            continue;
        }

        /* Collect until newline or line buffer full. */
        if (b == '\r' || b == '\n') {
            if (m_line_len > 0) {
                m_line[m_line_len] = '\0';

                /* Make a local copy and reset the buffer before parsing. */
                char tmp[LINE_SIZE];
                std::strncpy(tmp, m_line, LINE_SIZE - 1);
                tmp[LINE_SIZE - 1] = '\0';

                m_line_len = 0;
                m_line[0] = '\0';

                /* Dispatch via the command manager framework. */
                CommandManager::instance().processLine(tmp);
            }
        } else if (m_line_len < LINE_SIZE - 1) {
            m_line[m_line_len++] = static_cast<char>(b);
        } else {
            /* Never execute a silently truncated command. */
            m_line_len = 0;
            m_line[0] = '\0';
            m_discard_line = true;
        }
    }
}

} // namespace Inverter

/* TIME_DOMAIN: SHELL_UART_RX_ISR
 *   Byte-by-byte reception from USB-UART bridge.  ISR context, minimal work.
 * CODEGEN: Keep transport hook; codegen may add additional command sources
 *   (CAN, USB CDC, etc.) without changing this ISR.
 */
extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef* huart) {
    if (huart != nullptr && huart->Instance == USART3) {
        Inverter::commandShell().onRxComplete();
    }
}
