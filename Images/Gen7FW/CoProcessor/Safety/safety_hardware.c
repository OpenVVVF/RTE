#include "safety_hardware.h"
#include "safety_heartbeat.h"

#include "main.h"
#include "usbd_cdc_if.h"

static SafetyPolicy policy;
static SafetyHeartbeat heartbeat;
static bool planned_reset;
static bool clear_level_previous;
static uint16_t report_generation;
static SafetyClearResult clear_result;
static bool report_sent;
static uint32_t last_report_ms;
static SafetyState reported_state;
static uint32_t reported_faults;

static void report_if_due(uint32_t now_ms)
{
    if (policy.state != reported_state || policy.faults != reported_faults) {
        ++report_generation;
        reported_state = policy.state;
        reported_faults = policy.faults;
        report_sent = false;
    }
    if (!report_sent || (uint32_t)(now_ms - last_report_ms) >= 500u) {
        const SafetyFaultFrame frame = {
            .faults = policy.faults,
            .generation = report_generation,
            .state = (uint8_t)policy.state,
            .clear_result = (uint8_t)clear_result
        };
        CDC_Bridge_QueueSafetyStatus(&frame);
        last_report_ms = now_ms;
        report_sent = true;
    }
}

/* The PWR1 and PWR2 switches are in series on the Chassis2 gate board.
 * PC7 low opens PWR2 even if the main MCU leaves PWR1 on. */
void SafetyHardware_ForceOff(void)
{
    /* This path also runs from exception handlers. Use only register writes:
     * HAL state or interrupts may already be unusable. */
    __HAL_RCC_GPIOC_CLK_ENABLE();
    (void)RCC->AHB2ENR;
    GPIOC->BSRR = (uint32_t)GATE_DRIVE_PWR_ENABLE_Pin << 16;
    GPIOC->OTYPER &= ~(uint32_t)GATE_DRIVE_PWR_ENABLE_Pin;
    GPIOC->MODER = (GPIOC->MODER & ~(3u << (7u * 2u))) |
                   (1u << (7u * 2u));
}

static SafetyInputs read_inputs(uint32_t now_ms)
{
    return (SafetyInputs){
        .own_power_feedback = HAL_GPIO_ReadPin(GATE_DRIVE_PWR2_FEEDBACK_GPIO_Port,
                                               GATE_DRIVE_PWR2_FEEDBACK_Pin) == GPIO_PIN_SET,
        .main_power_feedback = HAL_GPIO_ReadPin(GATE_DRIVE_PWR1_FEEDBACK_GPIO_Port,
                                                GATE_DRIVE_PWR1_FEEDBACK_Pin) == GPIO_PIN_SET,
        .gate_fault = HAL_GPIO_ReadPin(GATE_DRIVER_FAULT_IN_GPIO_Port,
                                       GATE_DRIVER_FAULT_IN_Pin) == GPIO_PIN_RESET,
        .main_alive = SafetyHeartbeat_Update(
            &heartbeat,
            HAL_GPIO_ReadPin(INTERMCU_SYNC_LINE_GPIO_Port,
                             INTERMCU_SYNC_LINE_Pin) == GPIO_PIN_SET,
            now_ms)
    };
}

static void apply_outputs(void)
{
    HAL_GPIO_WritePin(GATE_DRIVE_PWR_ENABLE_GPIO_Port, GATE_DRIVE_PWR_ENABLE_Pin,
                      SafetyPolicy_PowerEnabled(&policy) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void SafetyHardware_Init(void)
{
    SafetyPolicy_Init(&policy);
    planned_reset = false;
    clear_level_previous = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_SET;
    report_generation = 0u;
    clear_result = SAFETY_CLEAR_NONE;
    report_sent = false;
    reported_state = policy.state;
    reported_faults = policy.faults;
    SafetyHardware_ForceOff();
    SafetyHeartbeat_Init(&heartbeat,
                         HAL_GPIO_ReadPin(INTERMCU_SYNC_LINE_GPIO_Port,
                                          INTERMCU_SYNC_LINE_Pin) == GPIO_PIN_SET,
                         HAL_GetTick());
    apply_outputs();
}

void SafetyHardware_Update(void)
{
    const uint32_t now_ms = HAL_GetTick();
    if (planned_reset) {
        SafetyHardware_ForceOff();
        if (HAL_GPIO_ReadPin(GATE_DRIVE_PWR2_FEEDBACK_GPIO_Port,
                             GATE_DRIVE_PWR2_FEEDBACK_Pin) == GPIO_PIN_SET) {
            report_if_due(now_ms);
            return; /* Wait for the switch to open before ordinary POST. */
        }
        planned_reset = false;
    }
    const SafetyInputs inputs = read_inputs(now_ms);
    const bool clear_level = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_SET;
    const bool clear_requested = clear_level && !clear_level_previous;
    clear_level_previous = clear_level;
    if (clear_requested) {
        if (policy.state != SAFETY_FAULT_LATCHED) {
            clear_result = SAFETY_CLEAR_NO_FAULT;
        } else if (SafetyPolicy_TryClear(&policy, inputs)) {
            clear_result = SAFETY_CLEAR_ACCEPTED;
        } else {
            clear_result = SAFETY_CLEAR_REFUSED;
        }
        ++report_generation;
        report_sent = false;
    }
    SafetyPolicy_Update(&policy, inputs, now_ms);
    if (!clear_requested && policy.state == SAFETY_INHIBITED &&
        !inputs.own_power_feedback) {
        /* Limited POST: this path confirms the PWR2 feedback is off before
         * enabling it. Extended RAM/ADC/clock POST is future work. */
        SafetyPolicy_PostPassed(&policy);
        (void)SafetyPolicy_RequestArm(&policy, inputs, now_ms);
    }
    apply_outputs();
    report_if_due(now_ms);
}

void SafetyHardware_PlanMainReset(void)
{
    SafetyHardware_ForceOff();
    if (policy.state == SAFETY_FAULT_LATCHED) {
        return;
    }
    SafetyPolicy_Init(&policy);
    SafetyHeartbeat_Init(&heartbeat,
                         HAL_GPIO_ReadPin(INTERMCU_SYNC_LINE_GPIO_Port,
                                          INTERMCU_SYNC_LINE_Pin) == GPIO_PIN_SET,
                         HAL_GetTick());
    planned_reset = true;
    clear_result = SAFETY_CLEAR_NONE;
    report_sent = false;
}

const SafetyPolicy *SafetyHardware_Status(void)
{
    return &policy;
}
