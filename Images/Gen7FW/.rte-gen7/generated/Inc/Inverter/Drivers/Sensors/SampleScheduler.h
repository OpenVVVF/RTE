#pragma once

#include <cstdint>

namespace Inverter {

/**
 * @brief Pluggable trigger scheduler for phase-current micro-bursts.
 *
 * Different modulation strategies (SVPWM, SHE-PWM, synchronous PWM, six-step)
 * need different ways to schedule ADC triggers.  This interface abstracts the
 * mechanism so the control loop can ask for "the next clean sample point"
 * without knowing how it is implemented.
 *
 * The default implementation uses TIM1 output-compare channel 4 for center-
 * aligned PWM.  Future SHE-PWM implementations can use TIM2/TIM5 compare
 * events or a software scheduler fed by the VoltageVectorSchedule.
 */
class SampleScheduler {
public:
    virtual ~SampleScheduler() = default;

    /**
     * @brief Schedule the next ADC trigger at the given TIM1 counter value.
     *
     * @param ccr4_ticks  TIM1 counter tick where the micro-burst should start.
     * @param arr         TIM1 auto-reload value.
     * @return true if the trigger was successfully scheduled.
     */
    virtual bool scheduleNextSample(uint32_t ccr4_ticks, uint32_t arr) = 0;

    /**
     * @brief Fall back to the legacy fixed trigger when no clean window exists.
     */
    virtual void scheduleFallback() = 0;
};

/**
 * @brief TIM1-OC4-based scheduler for center-aligned PWM (SVPWM/SPWM).
 */
class Tim1SampleScheduler : public SampleScheduler {
public:
    bool scheduleNextSample(uint32_t ccr4_ticks, uint32_t arr) override;
    void scheduleFallback() override;
};

/**
 * @brief Global scheduler instance.  Swap to a different implementation to
 * change the trigger mechanism.
 */
SampleScheduler& sampleScheduler();

} // namespace Inverter
