#include "safety_hardware.h"
#include "usbd_cdc_if.h"
#include "main.h"

#include <assert.h>

GPIO_TypeDef test_gpio_c, test_gpio_b;
RCC_TypeDef test_rcc;
static uint32_t now_ms;
static bool own_feedback, main_feedback, heartbeat_level, gate_fault;
static SafetyFaultFrame last_report;

uint32_t HAL_GetTick(void) { return now_ms; }

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    (void)port;
    bool high = false;
    if (pin == GATE_DRIVE_PWR2_FEEDBACK_Pin) high = own_feedback;
    if (pin == GATE_DRIVE_PWR1_FEEDBACK_Pin) high = main_feedback;
    if (pin == GATE_DRIVER_FAULT_IN_Pin) high = !gate_fault;
    if (pin == INTERMCU_SYNC_LINE_Pin) high = heartbeat_level;
    return high ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState level)
{
    (void)port;
    assert(pin == GATE_DRIVE_PWR_ENABLE_Pin);
    assert((level == GPIO_PIN_SET) == SafetyPolicy_PowerEnabled(SafetyHardware_Status()));
}

void CDC_Bridge_QueueSafetyStatus(const SafetyFaultFrame *frame)
{
    last_report = *frame;
}

static void step(uint32_t time_ms, bool edge)
{
    now_ms = time_ms;
    if (edge) heartbeat_level = !heartbeat_level;
    SafetyHardware_Update();
}

static void arm(void)
{
    now_ms = 0u;
    own_feedback = false;
    main_feedback = true;
    heartbeat_level = false;
    gate_fault = false;
    SafetyHardware_Init();
    step(20u, true);
    step(40u, true);
    assert(SafetyHardware_Status()->state == SAFETY_POWERING);
    own_feedback = true;
    step(50u, false);
    assert(SafetyHardware_Status()->state == SAFETY_ARMED);
}

int main(void)
{
    /* Reproduces the original shell reboot: power falls without notice. */
    arm();
    main_feedback = false;
    step(51u, false);
    assert(SafetyHardware_Status()->trip_reason == SAFETY_TRIP_MAIN_POWER_FEEDBACK_LOST);
    const SafetyPolicy latched = *SafetyHardware_Status();
    SafetyHardware_PlanMainReset();
    assert(SafetyHardware_Status()->faults == latched.faults);
    assert(SafetyHardware_Status()->trip_inputs == latched.trip_inputs);

    arm();
    SafetyHardware_PlanMainReset();
    assert(test_gpio_c.BSRR == (uint32_t)GATE_DRIVE_PWR_ENABLE_Pin << 16);
    step(51u, false);
    own_feedback = false;
    /* The command can wait in the H7 shell while its old heartbeat runs. */
    for (uint32_t time_ms = 60u; time_ms <= 600u; time_ms += 20u) {
        step(time_ms, true);
        assert(SafetyHardware_Status()->state == SAFETY_INHIBITED);
        assert(SafetyHardware_Status()->faults == 0u);
    }
    main_feedback = false;
    gate_fault = true;
    step(610u, false);
    assert(!SafetyHardware_Status()->planned_reset);
    step(1200u, false);
    assert(SafetyHardware_Status()->faults == 0u);
    main_feedback = true;
    gate_fault = false;
    step(1220u, true);
    assert(SafetyHardware_Status()->state == SAFETY_INHIBITED);
    step(1240u, true);
    assert(SafetyHardware_Status()->state == SAFETY_POWERING);
    own_feedback = true;
    step(1250u, false);
    assert(SafetyHardware_Status()->state == SAFETY_ARMED);
    assert(last_report.faults == 0u && last_report.state == SAFETY_ARMED);
    /* A real loss after recovery still latches normally. */
    own_feedback = false;
    step(1260u, false);
    assert(SafetyHardware_Status()->trip_reason == SAFETY_TRIP_OWN_POWER_FEEDBACK_LOST);

    arm();
    SafetyHardware_PlanMainReset();
    step(200u, false); /* PWR2 stuck on: do not wait forever. */
    assert(SafetyHardware_Status()->trip_reason == SAFETY_TRIP_POWER_STUCK_ON);
    arm();
    SafetyHardware_PlanMainReset();
    own_feedback = false;
    step(1550u, false); /* A command which never resets cannot rearm. */
    assert(SafetyHardware_Status()->trip_reason == SAFETY_TRIP_MAIN_HEARTBEAT_LOST);

    /* RESET/APP block safety polling for 170 ms. PWR1 can already be high
     * again when polling resumes; the bridge witnessed NRST directly. */
    arm();
    SafetyHardware_PlanMainReset();
    SafetyHardware_MainResetAsserted();
    own_feedback = false;
    step(220u, false);
    assert(!SafetyHardware_Status()->planned_reset);
    assert(SafetyHardware_Status()->state == SAFETY_INHIBITED);
    step(240u, true);
    assert(SafetyHardware_Status()->state == SAFETY_INHIBITED);
    step(260u, true);
    assert(SafetyHardware_Status()->state == SAFETY_POWERING);
    assert(SafetyHardware_Status()->faults == 0u);
    return 0;
}
