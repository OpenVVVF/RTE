/* adc.h — SIL CubeMX-style header: peripheral handle externs for the ADCs. */
#ifndef SIL_ADC_H
#define SIL_ADC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

extern ADC_HandleTypeDef hadc1;
extern ADC_HandleTypeDef hadc2;
extern ADC_HandleTypeDef hadc3;

void MX_ADC1_Init(void);
void MX_ADC2_Init(void);
void MX_ADC3_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* SIL_ADC_H */
