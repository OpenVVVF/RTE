/*
 * sil_misc.cpp — small SIL replacements:
 *   Src/Inverter/Drivers/Logging/SupplyMonitor.cpp   (no PMIC rails here)
 *   Src/Inverter/Drivers/Sensors/SampleScheduler.cpp (TIM1-OC4 trigger model)
 */
#include "Inverter/Drivers/Logging/SupplyMonitor.h"
#include "Inverter/Drivers/Sensors/SampleScheduler.h"

#include "main.h"
#include "tim.h"

namespace Inverter {

/* --- Supply monitor: all rails are always good in SIL --------------------*/
bool supplyMonitorInit() { return true; }
void supplyMonitorUpdate() {}
void supplyMonitorPrintStatus() {}

/* --- Sample scheduler ------------------------------------------------------
 * The TIM1-OC4 trigger position does not affect the averaged plant sample
 * (the SIL models one clean sample per trigger), so scheduling reduces to
 * bookkeeping that register-level readers can observe. */
bool Tim1SampleScheduler::scheduleNextSample(uint32_t ccr4_ticks, uint32_t arr) {
    (void)arr;
    TIM1->CCR4 = ccr4_ticks;
    return true;
}

void Tim1SampleScheduler::scheduleFallback() {
    /* Bottom-of-triangle trigger position retained by default. */
}

static Tim1SampleScheduler s_scheduler;

SampleScheduler& sampleScheduler() {
    return s_scheduler;
}

} // namespace Inverter
