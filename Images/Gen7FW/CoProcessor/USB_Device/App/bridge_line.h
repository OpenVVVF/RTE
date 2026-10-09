#ifndef BRIDGE_LINE_H
#define BRIDGE_LINE_H

#include <stdbool.h>
#include <stdint.h>

enum { BRIDGE_SHELL_CANCEL = 0x18u, BRIDGE_SHELL_LINE_SIZE = 128u };

typedef struct {
    uint16_t tail;
    uint16_t length;
    uint16_t dropped;
    bool discard;
} BridgeLineChunk;

/* Stage an entire application line before forwarding any of it. A USB
 * disconnect mid-line must not leave the UART between line boundaries and
 * starve private safety reports. Bootloader bytes use the opaque DMA path.
 * Overlong lines are discarded through their delimiter or explicit cancel. */
static inline BridgeLineChunk BridgeLine_Next(const uint8_t *ring, uint16_t size,
                                              uint16_t head, uint16_t tail,
                                              bool discard)
{
    BridgeLineChunk result = {tail, 0u, 0u, discard};
    while (result.tail != head) {
        if (result.discard) {
            const uint8_t byte = ring[result.tail];
            if (byte == BRIDGE_SHELL_CANCEL) {
                result.discard = false;
                continue;
            }
            result.tail = (result.tail + 1u) % size;
            ++result.dropped;
            if (byte == '\r' || byte == '\n') result.discard = false;
            continue;
        }
        uint16_t scan = result.tail;
        uint16_t length = 0u;
        while (scan != head && length < BRIDGE_SHELL_LINE_SIZE) {
            const uint8_t byte = ring[scan];
            scan = (scan + 1u) % size;
            ++length;
            if (byte == '\r' || byte == '\n') {
                const uint16_t contiguous = head > result.tail
                    ? head - result.tail : size - result.tail;
                result.length = length < contiguous ? length : contiguous;
                return result;
            }
        }
        if (length < BRIDGE_SHELL_LINE_SIZE) return result;
        result.discard = true;
    }
    return result;
}

#endif
