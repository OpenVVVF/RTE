/* tim.h — SIL CubeMX-style header: TIM peripheral handle externs. */
#ifndef SIL_TIM_H
#define SIL_TIM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

extern TIM_HandleTypeDef htim1;

void MX_TIM1_Init(void);
void HAL_TIM_MspPostInit(TIM_HandleTypeDef* htim);

#ifdef __cplusplus
}
#endif

#endif /* SIL_TIM_H */
