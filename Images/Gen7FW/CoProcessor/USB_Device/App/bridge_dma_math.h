#ifndef BRIDGE_DMA_MATH_H
#define BRIDGE_DMA_MATH_H

#include <stdint.h>

enum { BRIDGE_RX_DMA_SIZE = 8192U, BRIDGE_RX_DMA_HALF = BRIDGE_RX_DMA_SIZE / 2U };

/* Half/full interrupts count completed half-buffers. The pending count covers
 * a boundary crossed before the ISR gets CPU time. Both inputs are snapshots
 * taken with interrupts masked; DMA itself may continue advancing. */
static inline uint32_t BridgeDma_RxProduced(uint32_t completed_halves,
                                            uint32_t pending_halves,
                                            uint32_t remaining)
{
    const uint32_t halves = completed_halves + pending_halves;
    uint32_t position = BRIDGE_RX_DMA_SIZE - remaining;
    if (position == BRIDGE_RX_DMA_SIZE) position = 0U;
    const uint32_t base = halves * BRIDGE_RX_DMA_HALF;
    return (base & ~(BRIDGE_RX_DMA_SIZE - 1U)) + position +
           (((base & BRIDGE_RX_DMA_HALF) && position < BRIDGE_RX_DMA_HALF)
                ? BRIDGE_RX_DMA_SIZE : 0U);
}

/* In app mode, stop every block at the first command-line boundary so a
 * queued safety frame can be inserted before the next host command. ROM
 * bootloader traffic remains an opaque byte stream. */
static inline uint16_t BridgeDma_TxChunk(const uint8_t *bytes,
                                          uint16_t available,
                                          uint8_t bootloader)
{
    if (!bootloader) {
        for (uint16_t i = 0; i < available; ++i) {
            if (bytes[i] == '\r' || bytes[i] == '\n') return i + 1U;
        }
    }
    return available;
}

#endif
