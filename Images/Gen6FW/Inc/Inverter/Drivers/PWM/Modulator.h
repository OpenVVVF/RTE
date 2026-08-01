#pragma once

/**
 * @brief Swappable modulation strategy slot (multi-modulator architecture).
 *
 * A Modulator turns a voltage request (or its own internal reference, e.g.
 * the open-loop SPWM ramp) into TIM1 output state.  It never touches GPIO or
 * TIM1 configuration: the pwm.cpp driver owns all TIM1 registers, dead time,
 * break/MOE and preload semantics.  On the STM32H723 the gate pins PE8-PE13
 * are TIM1-only alternate functions, so TIM1 owns the gate outputs in every
 * mode; future synchronous modulators (SHEPWM) will drive TIM1 forced-output
 * modes from their own timebase instead of re-muxing pins.
 *
 * Two clocking styles exist:
 *  - Externally clocked (SVPWM): the control loop calls update() + commit()
 *    each control period.  runsInPwmIsr() == false.
 *  - Self-clocked (SPWM): the TIM1 update ISR calls update() + commit().
 *    runsInPwmIsr() == true.
 *
 * commit() writes CCR1..3 through the driver; ARPE + OC preload (enabled in
 * MX_TIM1_Init) make the commit atomic at the next TIM1 update event.
 */

#include <cstdint>

namespace Inverter {

class Modulator {
public:
    virtual ~Modulator() = default;

    /**
     * @brief Compute this period's outputs.
     *
     * Voltage-driven modulators consume (valpha_v, vbeta_v, vdc_v);
     * self-referenced ones (SPWM) ignore the arguments.
     */
    virtual void update(float valpha_v, float vbeta_v, float vdc_v) = 0;

    /**
     * @brief Write the outputs computed by update() to TIM1 via the driver.
     */
    virtual void commit() = 0;

    /**
     * @brief Take over the TIM1 outputs.
     *
     * theta_e_rad / modulation_index let synchronous modulators phase-lock;
     * async modulators ignore them.  Timer/IRQ configuration (RCR, NVIC)
     * stays with the driver call sites, not here.
     */
    virtual bool enter(float theta_e_rad, float modulation_index) = 0;

    /**
     * @brief Release the TIM1 outputs and reset internal pattern state.
     */
    virtual void exit() = 0;

    /**
     * @brief true if the TIM1 update ISR should drive update() + commit().
     */
    virtual bool runsInPwmIsr() const = 0;

    virtual const char* name() const = 0;
};

/* The single modulation slot.  Step 1 of the multi-modulator plan: hard-wired
 * by the existing call sites (FOC enable -> SVPWM, open-loop start -> SPWM);
 * a future supervisor will arbitrate transitions with hysteresis and
 * safe-point handoff. */
Modulator* activeModulator();
void setActiveModulator(Modulator* m);

Modulator& svpwmModulator();
Modulator& spwmModulator();
Modulator& shepwmModulator();

/* SPWM introspection / control for shell commands and calibrators
 * (forwarded by the legacy PWM_* API in pwm.cpp). */
bool spwmIsRunning();
float spwmAngleRad();
uint32_t spwmElectricalCycles();
void spwmResetElectricalCycles();
void spwmSetParams(float fundamental_freq_hz, float modulation_index);
float spwmFundamentalFreqHz();
float spwmModulationIndex();

/* SHEPWM control / introspection (shell bring-up; supervisor later).
 * shepwmSetPattern builds the event table for (fe_hz, mi) into the inactive
 * ping-pong buffer; while running it swaps at the next electrical-cycle wrap. */
bool shepwmIsRunning();
void shepwmSetPattern(float fe_hz, float mi);
float shepwmFrequencyHz();
float shepwmModulationIndex();
uint32_t shepwmWrapCount();
uint32_t shepwmEdgeCount();
/* Runtime N-pulse mode (no tables): npq cells per quarter cycle, each HIGH
 * for the middle `duty` fraction (centered notch of width 1-duty). */
void shepwmSetPulsePattern(float fe_hz, uint32_t npq, float duty);
uint32_t shepwmPulseCount();   /* 0 = SHE table mode */
float shepwmDuty();

} // namespace Inverter
