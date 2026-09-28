#include "../bridge_dma_math.h"

#include <assert.h>
#include <stdint.h>

int main(void)
{
    assert(BridgeDma_RxProduced(0, 0, BRIDGE_RX_DMA_SIZE) == 0U);
    assert(BridgeDma_RxProduced(0, 0, BRIDGE_RX_DMA_HALF + 1U) ==
           BRIDGE_RX_DMA_HALF - 1U);
    assert(BridgeDma_RxProduced(0, 1, BRIDGE_RX_DMA_HALF) ==
           BRIDGE_RX_DMA_HALF);
    assert(BridgeDma_RxProduced(1, 0, BRIDGE_RX_DMA_HALF - 6U) ==
           BRIDGE_RX_DMA_HALF + 6U);
    assert(BridgeDma_RxProduced(1, 1, BRIDGE_RX_DMA_SIZE) ==
           BRIDGE_RX_DMA_SIZE);
    assert(BridgeDma_RxProduced(1, 1, 0U) == BRIDGE_RX_DMA_SIZE);
    assert(BridgeDma_RxProduced(2, 0, BRIDGE_RX_DMA_SIZE - 10U) ==
           BRIDGE_RX_DMA_SIZE + 10U);
    assert(BridgeDma_RxProduced(0, 2, BRIDGE_RX_DMA_SIZE - 100U) ==
           BRIDGE_RX_DMA_SIZE + 100U);
    assert(BridgeDma_RxProduced(1048576U, 0, BRIDGE_RX_DMA_SIZE - 100U)
           == 100U); /* 32-bit byte counter wraps without losing deltas. */

    const uint8_t commands[] = {'a', 'b', '\n', 'c', 'd'};
    assert(BridgeDma_TxChunk(commands, sizeof(commands), 0) == 3U);
    assert(BridgeDma_TxChunk(commands, sizeof(commands), 1) == 5U);
    assert(BridgeDma_TxChunk(commands + 3, 2U, 0) == 2U);
    const uint8_t crlf[] = {'a', '\r', '\n', 'b'};
    assert(BridgeDma_TxChunk(crlf, sizeof(crlf), 0) == 2U);
    return 0;
}
