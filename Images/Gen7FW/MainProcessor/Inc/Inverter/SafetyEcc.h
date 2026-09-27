#ifndef INVERTER_SAFETY_ECC_H
#define INVERTER_SAFETY_ECC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Detect RAM ECC via the H723 ECC IRQ and flash ECC status in the main loop.
 * Both paths halt with PWM broken and gate power removed on detection. */
void SafetyEcc_Init(void);
void SafetyEcc_Check(void);
void ECC_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif
