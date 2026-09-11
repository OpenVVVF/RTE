/* fdcan.h — SIL CubeMX-style header: FDCAN peripheral handle externs. */
#ifndef SIL_FDCAN_H
#define SIL_FDCAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

extern FDCAN_HandleTypeDef hfdcan1;
extern FDCAN_HandleTypeDef hfdcan2;

void MX_FDCAN1_Init(void);
void MX_FDCAN2_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* SIL_FDCAN_H */
