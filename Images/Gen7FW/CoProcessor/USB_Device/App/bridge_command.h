#ifndef BRIDGE_COMMAND_H
#define BRIDGE_COMMAND_H

#include <stdbool.h>
#include <stdint.h>

/* Match the H7 shell's case-insensitive, whitespace-delimited reboot command.
 * State follows bytes sent to UART, across USB packets and TX ring wrap.
 * Overlong lines are discarded by the H7's 128-byte line buffer. */
typedef struct {
    uint8_t length;
    uint8_t matched;
    bool trailing;
    bool invalid;
    bool terminated;
} BridgeCommand;

static inline bool BridgeCommand_Whitespace(uint8_t byte)
{
    return byte == ' ' || byte == '\t' || byte == '\v' || byte == '\f';
}

/* Inspect one app-mode DMA chunk, which ends at its first CR/LF. Use a copy
 * of the parser until a reboot's terminating chunk is authorized for TX. */
static inline bool BridgeCommand_Feed(BridgeCommand *command,
                                      const uint8_t *bytes, uint16_t count)
{
    static const char reboot[] = "reboot";
    bool reset = false;
    for (uint16_t i = 0u; i < count; ++i) {
        uint8_t byte = bytes[i];
        if (byte == 0x18u) { /* Shell CAN cancels startup/interrupted input. */
            *command = (BridgeCommand){0};
            continue;
        }
        if (byte == '\r' || byte == '\n') {
            reset = !command->invalid && command->matched == sizeof(reboot) - 1u;
            *command = (BridgeCommand){0};
            continue;
        }
        if (command->length < 128u) ++command->length;
        if (command->length == 128u) command->invalid = true;
        if (command->invalid || command->terminated) continue;
        /* The H7 copies its line into a C string before command dispatch. */
        if (byte == 0u) {
            command->terminated = true;
        } else if (BridgeCommand_Whitespace(byte)) {
            if (command->matched != 0u) command->trailing = true;
        } else {
            if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
            if (command->trailing || command->matched >= sizeof(reboot) - 1u ||
                byte != (uint8_t)reboot[command->matched]) {
                command->invalid = true;
            } else {
                ++command->matched;
            }
        }
    }
    return reset;
}

#endif
