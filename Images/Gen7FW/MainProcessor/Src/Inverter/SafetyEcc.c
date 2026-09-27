#include "Inverter/SafetyEcc.h"

#include "Inverter/SafetyLink.h"
#include "main.h"

#include <stdint.h>

/* STM32H723 RAMECC monitor register groups, in the order defined by its
 * CMSIS device header. Include every monitor; any correctable or detected
 * double-bit error is treated as loss of trustworthy control memory. */
static RAMECC_MonitorTypeDef *const monitors[] = {
    RAMECC1_Monitor1, RAMECC1_Monitor2, RAMECC1_Monitor3,
    RAMECC1_Monitor4, RAMECC1_Monitor5, RAMECC1_Monitor6,
    RAMECC2_Monitor1, RAMECC2_Monitor2, RAMECC2_Monitor3,
    RAMECC3_Monitor1, RAMECC3_Monitor2
};

static void stop_on_ecc(void)
{
    SafetyLink_EmergencyStop();
    __disable_irq();
    for (;;) {
        /* Reset is required after an ECC event. */
    }
}

static uint32_t ram_ecc_status(void)
{
    uint32_t status = 0u;
    for (uint32_t i = 0u; i < sizeof(monitors) / sizeof(monitors[0]); ++i) {
        status |= monitors[i]->SR;
    }
    return status & (RAMECC_SR_SEDCF | RAMECC_SR_DEDF | RAMECC_SR_DEBWDF);
}

void SafetyEcc_Init(void)
{
    if (ram_ecc_status() != 0u) {
        stop_on_ecc();
    }
    for (uint32_t i = 0u; i < sizeof(monitors) / sizeof(monitors[0]); ++i) {
        monitors[i]->CR |= RAMECC_CR_ECCSEIE | RAMECC_CR_ECCDEIE |
                           RAMECC_CR_ECCDEBWIE;
    }
    RAMECC1->IER |= RAMECC_IER_GIE | RAMECC_IER_GECCSEIE |
                    RAMECC_IER_GECCDEIE | RAMECC_IER_GECCDEBWIE;
    RAMECC2->IER |= RAMECC_IER_GIE | RAMECC_IER_GECCSEIE |
                    RAMECC_IER_GECCDEIE | RAMECC_IER_GECCDEBWIE;
    RAMECC3->IER |= RAMECC_IER_GIE | RAMECC_IER_GECCSEIE |
                    RAMECC_IER_GECCDEIE | RAMECC_IER_GECCDEBWIE;
    HAL_NVIC_SetPriority(ECC_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(ECC_IRQn);
}

void SafetyEcc_Check(void)
{
    if (ram_ecc_status() != 0u ||
        (FLASH->SR1 & (FLASH_SR_SNECCERR | FLASH_SR_DBECCERR)) != 0u) {
        stop_on_ecc();
    }
}

void ECC_IRQHandler(void)
{
    /* If the shared ECC vector fires, halt even if a status flag was lost. */
    stop_on_ecc();
}
