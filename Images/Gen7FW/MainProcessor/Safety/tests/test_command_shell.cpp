#include "Inverter/Control/CommandShell.h"
#include "Inverter/Control/CoprocessorFaults.h"
#include "Inverter/Command/CommandManager.h"
#include "usart.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

UART_HandleTypeDef huart3{HAL_UART_STATE_READY, USART3};
static uint8_t* rx_target;
static unsigned flush_count, executed;
static bool registered, fail_first_arm;
static std::vector<std::string> messages;

void test_uart_flush() {
    assert(registered); /* The final startup flush follows registration. */
    ++flush_count;
}
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef* uart, uint8_t* target,
                                      uint16_t count) {
    assert(count == 1u);
    if (fail_first_arm) { fail_first_arm = false; return HAL_BUSY; }
    rx_target = target;
    uart->RxState = HAL_UART_STATE_BUSY_RX;
    return HAL_OK;
}
namespace Telemetry {
void printf(const char* format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    messages.emplace_back(message);
}
}
namespace Inverter {
CoprocessorFaults& CoprocessorFaults::instance() { static CoprocessorFaults f; return f; }
bool CoprocessorFaults::consumeLine(const char* line, size_t length) {
    return length >= 4u && std::memcmp(line, "!SF1", 4u) == 0;
}
}
class ProbeCommand final : public CommandInterface {
public:
    ProbeCommand() : CommandInterface("probe", "test") {}
    void execute(const ArgValue*, CommandContext&) override { ++executed; }
};
void initializeCommands() {
    static ProbeCommand command;
    CommandManager::instance().registerCommand(&command);
    registered = true;
}
static void receive(Inverter::CommandShell& shell, const std::string& bytes) {
    for (unsigned char byte : bytes) {
        assert(rx_target);
        *rx_target = byte;
        huart3.RxState = HAL_UART_STATE_READY;
        shell.onRxComplete();
    }
}
static bool saw(const char* fragment) {
    for (const auto& message : messages)
        if (message.find(fragment) != std::string::npos) return true;
    return false;
}

int main() {
    Inverter::CommandShell shell;
    assert(shell.init());
    assert(flush_count == 1u);

    /* New input cancels both garbage and a complete command lacking only its
     * terminator. Resynchronizing must not accidentally execute that command. */
    receive(shell, "probe");
    shell.poll();
    receive(shell, "\x18\nprobe\n");
    shell.poll();
    assert(executed == 1u);
    assert(!saw("Unknown command"));

    receive(shell, "startup-garbage");
    shell.poll();
    receive(shell, "\x18\nprobe\n");
    shell.poll();
    assert(executed == 2u);
    assert(!saw("Unknown command"));

    /* Recovery after a consumed leading newline previously swallowed the
     * first real command. CAN also releases the recovery discard state. */
    receive(shell, "\n");
    shell.recover();
    shell.poll();
    assert(flush_count == 2u);
    assert(saw("UART input discarded"));
    receive(shell, "\x18probe\n"); /* Works even if the extra newline is lost. */
    shell.poll();
    assert(executed == 3u);

    /* A genuinely damaged command is dropped, not reconstructed/retried. */
    receive(shell, "pro");
    shell.recover();
    receive(shell, "be\n");
    shell.poll();
    assert(executed == 3u);
    receive(shell, "probe\n"); /* Bare terminal commands remain supported. */
    shell.poll();
    assert(executed == 4u);

    receive(shell, std::string(140, 'x'));
    shell.poll();
    receive(shell, "\x18\nprobe\n");
    shell.poll();
    assert(executed == 5u);

    receive(shell, std::string(300, 'x'));
    shell.poll();
    assert(shell.rxDroppedCount() != 0u);
    receive(shell, "\x18\nprobe\n");
    shell.poll();
    assert(executed == 6u);
    assert(!saw("Unknown command"));

    Inverter::CommandShell retry;
    fail_first_arm = true;
    huart3.RxState = HAL_UART_STATE_READY;
    assert(!retry.init());
    retry.poll();
    receive(retry, "\x18\nprobe\n");
    retry.poll();
    assert(executed == 7u);
    assert(retry.rxRearmFailureCount() == 1u);
}
