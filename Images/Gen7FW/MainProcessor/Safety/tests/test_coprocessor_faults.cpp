#include "Inverter/Control/CoprocessorFaults.h"
#include "main.h"

#include <cassert>

GPIO_TypeDef test_gpio_d = {1};
GPIO_TypeDef test_gpio_c = {2};
TIM_TypeDef test_tim1 = {0};
static uint32_t now_ms;
static GPIO_PinState clear_level = GPIO_PIN_RESET;

uint32_t HAL_GetTick(void) { return now_ms; }
void HAL_GPIO_WritePin(GPIO_TypeDef*, uint16_t pin, GPIO_PinState state) {
    if (pin == COPROCESSOR_WAKEUP_Pin) clear_level = state;
}
namespace Telemetry {
void log(const char*, const char*) {}
void log(const char*, float) {}
void printf(const char*, ...) {}
}

int main() {
    auto& faults = Inverter::CoprocessorFaults::instance();
    assert(!faults.fresh());
    assert(!faults.requestClear());
    const SafetyFaultFrame sent{1u << 4, 7u, 3u, SAFETY_CLEAR_REFUSED};
    char line[SAFETY_FAULT_WIRE_LENGTH];
    SafetyFaultWire_Encode(&sent, line);
    assert(faults.consumeLine(line, 20u));
    assert(faults.fresh());
    assert(faults.hasFault());
    assert(faults.generation() == 7u);
    assert(faults.requestClear());
    assert(clear_level == GPIO_PIN_SET);
    assert(!faults.requestClear());
    now_ms = 100u;
    faults.service();
    assert(clear_level == GPIO_PIN_RESET);

    line[4] = 'F'; /* CRC must reject the mutated report. */
    assert(faults.consumeLine(line, 20u));
    assert(faults.generation() == 7u);
    now_ms = 1700u;
    assert(!faults.fresh());
    assert(!faults.requestClear());
    return 0;
}
