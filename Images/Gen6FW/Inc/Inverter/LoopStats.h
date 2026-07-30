#pragma once

#include <cstdint>

namespace Inverter {
namespace LoopStats {

/**
 * @brief Per-domain step counters for rate telemetry.
 *
 * Incremented at each generated-domain dispatch point, sampled and cleared
 * once per second by the main loop (see InverterMain.cpp).  ISR increments
 * are single 32-bit stores — atomic on Cortex-M; the once-per-second
 * read/clear can lose at most one tick per domain, which is fine for
 * telemetry.
 *
 * Domains:
 *  - app_loop : free-running main-loop application step (soft RT).
 *  - vsense   : voltage-sense step, currently also main-loop cadence.
 *  - tim_isr  : TIM1 PWM-update ISR (control law + modulation).
 *  - adc_isr  : ADC injected conversion-complete ISR (phase currents).
 */
extern volatile uint32_t app_loop;
extern volatile uint32_t vsense;
extern volatile uint32_t tim_isr;
extern volatile uint32_t adc_isr;

} // namespace LoopStats
} // namespace Inverter
