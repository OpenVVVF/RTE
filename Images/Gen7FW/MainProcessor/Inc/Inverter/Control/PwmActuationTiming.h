#pragma once

#include <cstdint>

namespace Inverter::PwmActuationTiming {
// A center-aligned timer transfers preloaded duties at the next extremum.
// The average voltage from those duties belongs to the middle of the following
// half-cycle. CNT/DIR are read when the angle is read, not at ISR entry.
inline float secondsToNextVoltageMidpoint(uint32_t arr, uint32_t cnt,
                                          bool counting_down, uint32_t psc,
                                          uint32_t timer_clock_hz) {
    if (arr == 0U || cnt > arr || timer_clock_hz == 0U) return 0.0f;
    const uint32_t ticks_to_update = counting_down ? cnt : arr - cnt;
    return (float(ticks_to_update) + 0.5f * float(arr)) *
           float(psc + 1U) / float(timer_clock_hz);
}
} // namespace Inverter::PwmActuationTiming
