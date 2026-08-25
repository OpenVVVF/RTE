#include "Inverter/Drivers/Sensors/SampleScheduler.h"

#include "tim.h"

namespace Inverter {

static Tim1SampleScheduler s_tim1_scheduler;

SampleScheduler& sampleScheduler() {
    return s_tim1_scheduler;
}

bool Tim1SampleScheduler::scheduleNextSample(uint32_t ccr4_ticks, uint32_t arr) {
    (void)arr;
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_4, ccr4_ticks);
    return true;
}

void Tim1SampleScheduler::scheduleFallback() {
    /* Legacy behavior: trigger near the bottom of the PWM triangle. */
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_4, 10U);
}

} // namespace Inverter
