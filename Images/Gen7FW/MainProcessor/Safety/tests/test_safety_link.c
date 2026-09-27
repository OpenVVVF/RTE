#include "Inverter/SafetyLink.h"
#include "main.h"

#include <assert.h>

GPIO_TypeDef test_gpio_d = {1};
GPIO_TypeDef test_gpio_c = {2};
TIM_TypeDef test_tim1 = {0};
static uint32_t now_ms;
static GPIO_PinState sync_level;
static GPIO_PinState power_level = GPIO_PIN_SET;
static GPIO_PinState reset_level = GPIO_PIN_SET;

uint32_t HAL_GetTick(void) { return now_ms; }

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
    if (port == &test_gpio_d && pin == COPROCESSOR_SYNC_Pin) sync_level = state;
    if (port == &test_gpio_c && pin == GATE_DRIVER_POWER_ENABLE_Pin) power_level = state;
    if (port == &test_gpio_d && pin == GATE_DRIVER_RESET_Pin) reset_level = state;
}

int main(void)
{
    SafetyLink_Init();
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
    return 0;
}
