#ifndef TEST_ECC_MAIN_H
#define TEST_ECC_MAIN_H

#include <stdint.h>

typedef struct { uint32_t CR; uint32_t SR; } RAMECC_MonitorTypeDef;
typedef struct { uint32_t IER; } RAMECC_TypeDef;
typedef struct { uint32_t SR1; } FLASH_TypeDef;

extern RAMECC_MonitorTypeDef test_monitors[11];
extern RAMECC_TypeDef test_units[3];
extern FLASH_TypeDef test_flash;

#define RAMECC1_Monitor1 (&test_monitors[0])
#define RAMECC1_Monitor2 (&test_monitors[1])
#define RAMECC1_Monitor3 (&test_monitors[2])
#define RAMECC1_Monitor4 (&test_monitors[3])
#define RAMECC1_Monitor5 (&test_monitors[4])
#define RAMECC1_Monitor6 (&test_monitors[5])
#define RAMECC2_Monitor1 (&test_monitors[6])
#define RAMECC2_Monitor2 (&test_monitors[7])
#define RAMECC2_Monitor3 (&test_monitors[8])
#define RAMECC3_Monitor1 (&test_monitors[9])
#define RAMECC3_Monitor2 (&test_monitors[10])
#define RAMECC1 (&test_units[0])
#define RAMECC2 (&test_units[1])
#define RAMECC3 (&test_units[2])
#define FLASH (&test_flash)

#define RAMECC_SR_SEDCF 1u
#define RAMECC_SR_DEDF 2u
#define RAMECC_SR_DEBWDF 4u
#define RAMECC_CR_ECCSEIE 4u
#define RAMECC_CR_ECCDEIE 8u
#define RAMECC_CR_ECCDEBWIE 16u
#define RAMECC_IER_GIE 1u
#define RAMECC_IER_GECCSEIE 2u
#define RAMECC_IER_GECCDEIE 4u
#define RAMECC_IER_GECCDEBWIE 8u
#define FLASH_SR_SNECCERR 0x02000000u
#define FLASH_SR_DBECCERR 0x04000000u
#define ECC_IRQn 145

void HAL_NVIC_SetPriority(int irq, int preempt, int sub);
void HAL_NVIC_EnableIRQ(int irq);
void HAL_NVIC_ClearPendingIRQ(int irq);
void HAL_NVIC_DisableIRQ(int irq);

#endif
