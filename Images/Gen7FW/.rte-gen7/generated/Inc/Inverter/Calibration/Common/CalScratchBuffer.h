#pragma once

#include <cstddef>
#include <cstdint>

namespace Inverter {
namespace CalScratch {

/**
 * @brief Shared scratch memory for calibration routines.
 *
 * Placed in AXISRAM (.dma_buffers) so it does not consume DTCM.  Only one
 * calibration routine runs at a time, so the buffer is safely reused across
 * routines.  Each caller must zero the portion it uses before starting.
 */
constexpr size_t SIZE = 32768U;

/** Raw buffer pointer. */
void* buffer();

/** Buffer size in bytes. */
size_t size();

/** Convenience typed accessor. */
template <typename T>
T* as() {
    return reinterpret_cast<T*>(buffer());
}

/** Zero the entire shared scratch buffer. */
void zero();

} // namespace CalScratch
} // namespace Inverter
