#include "Inverter/Calibration/Common/CalScratchBuffer.h"

#include <cstring>

namespace Inverter {
namespace CalScratch {

/* NOLOAD section: not zero-initialized by the C runtime, so callers must
 * zero the portion they use. */
static uint8_t s_cal_scratch[SIZE] __attribute__((section(".dma_buffers")));

void* buffer() {
    return s_cal_scratch;
}

size_t size() {
    return SIZE;
}

void zero() {
    std::memset(s_cal_scratch, 0, SIZE);
}

} // namespace CalScratch
} // namespace Inverter
