#include "Inverter/SafetyEcc.h"
#include "main.h"

#include <assert.h>
#include <stdlib.h>

RAMECC_MonitorTypeDef test_monitors[11];
RAMECC_TypeDef test_units[3];
FLASH_TypeDef test_flash;

static unsigned stops;
static unsigned reports;
static unsigned enabled;
static unsigned cleared;
static unsigned disabled;
static bool reported_ram;
static bool reported_flash;
static uint32_t reported_monitor;
static uint32_t reported_ram_status;
static uint32_t reported_flash_status;

void SafetyLink_EmergencyStop(void) { ++stops; }
void HAL_NVIC_SetPriority(int irq, int preempt, int sub)
{ assert(irq == ECC_IRQn && preempt == 0 && sub == 0); }
void HAL_NVIC_EnableIRQ(int irq) { assert(irq == ECC_IRQn); ++enabled; }
void HAL_NVIC_ClearPendingIRQ(int irq) { assert(irq == ECC_IRQn); ++cleared; }
void HAL_NVIC_DisableIRQ(int irq) { assert(irq == ECC_IRQn); ++disabled; }
void SafetyEcc_ReportFault(bool ram, bool flash, uint32_t monitor,
                           uint32_t ram_status, uint32_t flash_status)
{
    ++reports;
    reported_ram = ram;
    reported_flash = flash;
    reported_monitor = monitor;
    reported_ram_status = ram_status;
    reported_flash_status = flash_status;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const int scenario = atoi(argv[1]);
    if (scenario == 1) {
        test_flash.SR1 = FLASH_SR_SNECCERR;
    } else if (scenario == 3) {
        test_monitors[1].SR = RAMECC_SR_SEDCF | RAMECC_SR_DEDF;
    } else if (scenario == 4) {
        test_monitors[0].SR = RAMECC_SR_SEDCF | RAMECC_SR_DEDF;
        test_monitors[2].SR = RAMECC_SR_DEBWDF;
        SafetyEcc_ClearStartupStatus();
        assert(test_monitors[0].SR == 0 && test_monitors[2].SR == 0);
        SafetyEcc_Init();
        assert(enabled == 1 && stops == 0 && SafetyEcc_Check());
        assert(!SafetyEcc_HasLatchedFault());
        return 0;
    } else if (scenario == 5) {
        SafetyEcc_Init();
        assert(cleared == 1 && enabled == 1);
        ECC_IRQHandler();
        assert(stops == 0 && disabled == 0 && SafetyEcc_Check());
        assert(!SafetyEcc_HasLatchedFault());
        return 0;
    }
    SafetyEcc_Init();
    if (scenario == 0) {
        assert(enabled == 1 && stops == 0);
        test_monitors[0].SR = RAMECC_SR_SEDCF;
        assert(!SafetyEcc_Check());
        assert(reported_ram && !reported_flash);
        assert(reported_monitor == 1 && reported_ram_status == RAMECC_SR_SEDCF);
    } else if (scenario == 1) {
        assert(enabled == 0 && stops == 1);
        assert(!SafetyEcc_Check());
        assert(!reported_ram && reported_flash);
        assert(reported_flash_status == FLASH_SR_SNECCERR);
    } else if (scenario == 2) {
        assert(enabled == 1);
        test_monitors[0].SR = RAMECC_SR_DEDF;
        ECC_IRQHandler();
        assert(disabled == 1 && stops == 1);
        assert(!SafetyEcc_Check());
        assert(reported_ram && !reported_flash);
    } else {
        assert(scenario == 3);
        assert(test_monitors[1].SR == 0);
        assert(test_monitors[1].CR == 0);
        assert(enabled == 1 && stops == 0);
        assert(SafetyEcc_Check());
        test_monitors[2].SR = RAMECC_SR_DEDF;
        assert(!SafetyEcc_Check());
        assert(reported_ram && reported_monitor == 3);
    }
    assert(SafetyEcc_HasLatchedFault());
    assert(reports == 1);
    assert(!SafetyEcc_Check());
    assert(reports == 1);
    return 0;
}
