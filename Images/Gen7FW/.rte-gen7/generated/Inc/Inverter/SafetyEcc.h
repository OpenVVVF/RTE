#ifndef INVERTER_SAFETY_ECC_H
#define INVERTER_SAFETY_ECC_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Arm RAM ECC after application diagnostics are initialized. An ECC event
 * latches shutdown and leaves only the diagnostic shell/telemetry loop alive. */
void SafetyEcc_Init(void);
void SafetyEcc_ClearStartupStatus(void);
bool SafetyEcc_Check(void);
bool SafetyEcc_HasLatchedFault(void);
void ECC_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif
