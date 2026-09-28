#include "Inverter/SafetyEcc.h"

#include "Inverter/SafetyLink.h"
#include "main.h"

#include <stdint.h>

/* Only memories used by this image are safety relevant. The linker places no
 * code or data in ITCM; its monitor 2 reports 0x3 on this board at every boot
 * before the application has accessed it. Do not arm or poll that monitor. */
static const struct {
    RAMECC_MonitorTypeDef *regs;
    uint32_t number;
} monitors[] = {
    {RAMECC1_Monitor1, 1u}, {RAMECC1_Monitor3, 3u},
    {RAMECC1_Monitor4, 4u}, {RAMECC1_Monitor5, 5u},
    {RAMECC1_Monitor6, 6u}, {RAMECC2_Monitor1, 7u},
    {RAMECC2_Monitor2, 8u}, {RAMECC2_Monitor3, 9u},
    {RAMECC3_Monitor1, 10u}, {RAMECC3_Monitor2, 11u}
};

/* This bridge runs only from the application loop, after telemetry is ready. */
extern void SafetyEcc_ReportFault(bool ram, bool flash, uint32_t monitor,
                                  uint32_t ram_status, uint32_t flash_status);

static volatile bool ram_fault_latched;
static volatile bool flash_fault_latched;
static bool diagnostic_reported;
static uint32_t first_monitor;
static uint32_t first_ram_status;
static uint32_t first_flash_status;

static uint32_t ram_ecc_status(void)
{
    uint32_t status = 0u;
    for (uint32_t i = 0u; i < sizeof(monitors) / sizeof(monitors[0]); ++i) {
        const uint32_t flags = monitors[i].regs->SR &
            (RAMECC_SR_SEDCF | RAMECC_SR_DEDF | RAMECC_SR_DEBWDF);
        if (flags != 0u && first_ram_status == 0u) {
            first_monitor = monitors[i].number;
            first_ram_status = flags;
        }
        status |= flags;
    }
    return status;
}

void SafetyEcc_ClearStartupStatus(void)
{
    /* Called by Reset_Handler immediately after full-word RAM initialization.
     * These are sticky status bits from memory before its ECC bits were valid;
     * all later events remain latched for SafetyEcc_Init/Check. */
    for (uint32_t i = 0u; i < sizeof(monitors) / sizeof(monitors[0]); ++i) {
        monitors[i].regs->SR = 0u;
    }
    RAMECC1_Monitor2->SR = 0u;
}

void SafetyEcc_Init(void)
{
    /* Do not arm or poll the unused ITCM monitor. Clear its status before
     * enabling the domain-wide interrupt. */
    RAMECC1_Monitor2->CR &= ~(RAMECC_CR_ECCSEIE | RAMECC_CR_ECCDEIE |
                              RAMECC_CR_ECCDEBWIE);
    RAMECC1_Monitor2->SR = 0u;
    /* Application initialization can still encounter real RAM errors before
     * the diagnostics shell exists. Keep those detected statuses. */
    if (ram_ecc_status() != 0u) {
        ram_fault_latched = true;
    }
    first_flash_status = FLASH->SR1 &
        (FLASH_SR_SNECCERR | FLASH_SR_DBECCERR);
    if (first_flash_status != 0u) {
        flash_fault_latched = true;
    }
    if (ram_fault_latched || flash_fault_latched) {
        SafetyLink_EmergencyStop();
        return;
    }
    for (uint32_t i = 0u; i < sizeof(monitors) / sizeof(monitors[0]); ++i) {
        monitors[i].regs->CR |= RAMECC_CR_ECCSEIE | RAMECC_CR_ECCDEIE |
                           RAMECC_CR_ECCDEBWIE;
    }
    /* Global enables also include the unused ITCM monitor. The enabled
     * per-monitor interrupts above cover the banks this image actually uses. */
    RAMECC1->IER = 0u;
    RAMECC2->IER = 0u;
    RAMECC3->IER = 0u;
    HAL_NVIC_SetPriority(ECC_IRQn, 0, 0);
    HAL_NVIC_ClearPendingIRQ(ECC_IRQn);
    HAL_NVIC_EnableIRQ(ECC_IRQn);
}

bool SafetyEcc_Check(void)
{
    if (ram_ecc_status() != 0u) {
        ram_fault_latched = true;
    }
    const uint32_t flash_status = FLASH->SR1 &
        (FLASH_SR_SNECCERR | FLASH_SR_DBECCERR);
    if (flash_status != 0u) {
        flash_fault_latched = true;
        if (first_flash_status == 0u) first_flash_status = flash_status;
    }
    if (!ram_fault_latched && !flash_fault_latched) return true;

    if (!diagnostic_reported) {
        SafetyLink_EmergencyStop();
        SafetyEcc_ReportFault(ram_fault_latched, flash_fault_latched,
                              first_monitor, first_ram_status,
                              first_flash_status);
        diagnostic_reported = true;
    }
    return false;
}

bool SafetyEcc_HasLatchedFault(void)
{
    return ram_fault_latched || flash_fault_latched;
}

void ECC_IRQHandler(void)
{
    /* The shared vector can be pending from startup even after flags clear.
     * A real event leaves a sticky status bit; only that is a fault. */
    if (ram_ecc_status() == 0u) return;
    ram_fault_latched = true;
    SafetyLink_EmergencyStop();
    HAL_NVIC_DisableIRQ(ECC_IRQn);
}
