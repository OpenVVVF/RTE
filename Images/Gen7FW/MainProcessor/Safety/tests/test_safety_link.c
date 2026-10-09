#include "Inverter/SafetyLink.h"
#include "main.h"

#include <assert.h>

GPIO_TypeDef test_gpio_d = {1};
GPIO_TypeDef test_gpio_c = {2};
TIM_TypeDef test_tim1 = {0, 0, 0};
TIM_TypeDef *test_tim1_instance = &test_tim1;
static uint32_t now_ms;
static GPIO_PinState sync_level;
static GPIO_PinState power_level = GPIO_PIN_SET;
static GPIO_PinState reset_level = GPIO_PIN_SET;

uint32_t HAL_GetTick(void) { return now_ms; }

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    if (port == &test_gpio_c && pin == GATE_DRIVER_POWER_ENABLE_Pin) return power_level;
    if (port == &test_gpio_d && pin == GATE_DRIVER_RESET_Pin) return reset_level;
    return GPIO_PIN_RESET;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
    if (port == &test_gpio_d && pin == COPROCESSOR_SYNC_Pin) sync_level = state;
    if (port == &test_gpio_c && pin == GATE_DRIVER_POWER_ENABLE_Pin) power_level = state;
    if (port == &test_gpio_d && pin == GATE_DRIVER_RESET_Pin) reset_level = state;
}

int main(void)
{
    SafetyLink_Init();
    assert(SafetyLink_IsEnabled());
    assert(sync_level == GPIO_PIN_RESET);
    now_ms = 19;
    SafetyLink_Tick();
    assert(sync_level == GPIO_PIN_RESET);
    now_ms = 20;
    SafetyLink_Tick();
    assert(sync_level == GPIO_PIN_SET);
    now_ms = 40;
    SafetyLink_Tick();
    assert(sync_level == GPIO_PIN_RESET);

    SafetyLink_MainLoop(false);
    now_ms = 1000;
    SafetyLink_Tick();
    assert(sync_level == GPIO_PIN_SET); /* Idle work may exceed 500 ms. */

    SafetyLink_MainLoop(true);
    now_ms = 1501;
    SafetyLink_Tick();
    assert(sync_level == GPIO_PIN_SET); /* Active loop stalled; no edge. */
    SafetyLink_MainLoop(true);
    SafetyLink_Tick();
    assert(sync_level == GPIO_PIN_RESET);

    SafetyLink_EmergencyStop();
    assert(!SafetyLink_IsEnabled());
    assert(sync_level == GPIO_PIN_RESET);
    assert(power_level == GPIO_PIN_RESET);
    assert(reset_level == GPIO_PIN_RESET);
    assert(test_tim1.EGR == TIM_EGR_BG);
    now_ms = 2000;
    SafetyLink_Tick();
    assert(sync_level == GPIO_PIN_RESET);
    SafetyLink_Resume();
    now_ms = 2020;
    SafetyLink_Tick();
    assert(sync_level == GPIO_PIN_SET);

    /* Direct/static calibration PWM is active even without graph/FOC flags.
     * Idle MOE with no channels, and measurement with /RST low, are not. */
    test_tim1.BDTR = TIM_BDTR_MOE;
    assert(!SafetyLink_OutputsActive());
    test_tim1.CCER = TIM_CCER_CC1E;
    power_level = GPIO_PIN_SET;
    reset_level = GPIO_PIN_RESET;
    assert(!SafetyLink_OutputsActive());
    reset_level = GPIO_PIN_SET;
    assert(SafetyLink_OutputsActive());
    SafetyLink_MainLoop(false);
    now_ms += 501u;
    SafetyLink_Tick();
    assert(sync_level == GPIO_PIN_SET); /* Stalled direct-PWM loop loses edges. */
    SafetyLink_MainLoop(false);
    SafetyLink_Tick();
    assert(sync_level == GPIO_PIN_RESET);
    return 0;
}
