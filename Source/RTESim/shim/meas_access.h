#pragma once

/**
 * SFINAE helpers for reading plant::PlantMeas fields that MAY be absent
 * from a generated plant (the PlantCodegen contract leaves unconnected
 * channels out of the struct) + the shared bench constants both the
 * platform shim and the scheduler main need.
 *
 * C++17-compatible (no requires-expressions) so the scenario binary builds
 * with the same -std=c++17 the emitted firmware support headers (au.hh)
 * were proven with in twin/native.
 */

#include <type_traits>
#include <utility>

#include "plant_generated.h"

#define RTESIM_MEAS_ACCESSOR(Field)                                                        \
    template <typename M, typename = void> struct Has_##Field : std::false_type {};       \
    template <typename M>                                                                  \
    struct Has_##Field<M, std::void_t<decltype(std::declval<const M&>().Field)>>          \
        : std::true_type {};                                                               \
    template <typename PlantMeasT>                                                         \
    inline double Meas_##Field(const PlantMeasT& m, double fallback) {                    \
        if constexpr (Has_##Field<PlantMeasT>::value)                                      \
            return static_cast<double>(m.Field);                                           \
        return fallback;                                                                   \
    }

namespace rtesim_host {

// Encoder healthy-bounds mid/amp (Encoder-Loss report; identical literals in
// twin/native/src/platform_shim.cpp and gen7_bench_pmsm.json's Enc node).
inline constexpr double kEncMidSin = 32643.5;
inline constexpr double kEncAmpSin = 21220.5;
inline constexpr double kEncMidCos = 32536.5;
inline constexpr double kEncAmpCos = 21190.5;

} // namespace rtesim_host
