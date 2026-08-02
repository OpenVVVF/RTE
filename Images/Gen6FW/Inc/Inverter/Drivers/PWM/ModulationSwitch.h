#pragma once

#include <cstdint>

namespace Inverter {

/**
 * @brief Live modulator handoff primitives (ramp <-> pattern).
 *
 * These are the HAL-level switching operations behind both the 'handoff'
 * shell command and the codegen-facing platform_api.  Phase-locked in both
 * directions (angle capture + seed, level-matched pins); TIM1 owns the gate
 * outputs throughout, so dead time, MOE and the BKIN trip stay armed.
 */
enum class ModulationMode : uint8_t { Ramp = 0, Pattern = 1 };

ModulationMode modulationMode();

/* Ramp (open-loop SPWM) -> N-pulse pattern at the same electrical frequency.
 * Fails (returns false) if the ramp is not running or FOC is active. */
bool modulationToPattern(uint32_t pulses_per_quarter, float duty);

/* Pattern -> ramp, resuming at the pattern angle with the OL MI. */
bool modulationToRamp();

} // namespace Inverter
