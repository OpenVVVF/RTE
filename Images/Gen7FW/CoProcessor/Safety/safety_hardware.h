#ifndef COPROCESSOR_SAFETY_HARDWARE_H
#define COPROCESSOR_SAFETY_HARDWARE_H

#include "safety_policy.h"

/* Pin adapter for the Chassis2 ControlBoard/GateDriver schematics. */
void SafetyHardware_Init(void);
void SafetyHardware_Update(void);
void SafetyHardware_ForceOff(void);
/* Called before an intentional H7 reset by the USB bridge. A latched safety
 * fault is never cleared by this path. */
void SafetyHardware_PlanMainReset(void);
const SafetyPolicy *SafetyHardware_Status(void);

#endif
