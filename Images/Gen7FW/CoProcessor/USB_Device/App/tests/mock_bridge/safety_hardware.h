#include "safety_policy.h"
#include "safety_fault_wire.h"
const SafetyPolicy *SafetyHardware_Status(void);
void SafetyHardware_PlanMainReset(void);
void SafetyHardware_MainResetAsserted(void);
