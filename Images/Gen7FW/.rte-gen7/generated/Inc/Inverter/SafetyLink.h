#ifndef INVERTER_SAFETY_LINK_H
#define INVERTER_SAFETY_LINK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PD8 -> G474 PB9. Initialized only after GPIO setup. */
void SafetyLink_Init(void);
void SafetyLink_Tick(void);
void SafetyLink_MainLoop(bool actuating);
void SafetyLink_Stop(void);
/* Re-enable edges only after the main fault manager has no Critical fault. */
void SafetyLink_Resume(void);
void SafetyLink_EmergencyStop(void);

#ifdef __cplusplus
}
#endif

#endif
