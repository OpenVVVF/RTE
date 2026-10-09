#include "Inverter/Control/CoprocessorFaults.h"
#include "Inverter/Control/FaultManager.h"
#include "main.h"

#include <cassert>
#include <map>
#include <string>

GPIO_TypeDef test_gpio_d = {1};
GPIO_TypeDef test_gpio_c = {2};
TIM_TypeDef test_tim1 = {0, 0, 0};
TIM_TypeDef *test_tim1_instance = &test_tim1;
static uint32_t now_ms;
static GPIO_PinState clear_level = GPIO_PIN_RESET;
static std::map<std::string, std::string> strings;
static std::map<std::string, float> numbers;
static Inverter::FaultReason last_reason = Inverter::FaultReason::Unspecified;

namespace Inverter {
FaultManager& FaultManager::instance() { static FaultManager faults; return faults; }
void FaultManager::raise(FaultSource source, FaultReason reason) {
    assert(source == FaultSource::CoprocessorSafety);
    last_reason = reason;
}
}

uint32_t HAL_GetTick(void) { return now_ms; }
void HAL_GPIO_WritePin(GPIO_TypeDef*, uint16_t pin, GPIO_PinState state) {
    if (pin == COPROCESSOR_WAKEUP_Pin) clear_level = state;
}
namespace Telemetry {
void log(const char* key, const char* value) { strings[key] = value; }
void log(const char* key, float value) { numbers[key] = value; }
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
    assert(!faults.readyForActuation());
    faults.service(true);
    assert(last_reason == Inverter::FaultReason::CoprocessorTrip);
    assert(faults.generation() == 7u);
    assert(faults.requestClear());
    assert(clear_level == GPIO_PIN_SET);
    assert(!faults.requestClear());
    now_ms = 100u;
    faults.service();
    assert(clear_level == GPIO_PIN_RESET);
    assert(strings["coprocessor_clear_result"] == "pending");
    assert(!faults.requestClear()); /* Previous refused result is not a new ACK. */

    line[4] = 'F'; /* CRC must reject the mutated report. */
    assert(faults.consumeLine(line, 20u));
    assert(faults.generation() == 7u);
    now_ms = 1700u;
    assert(!faults.fresh());
    assert(!faults.requestClear());
    faults.service(true);
    assert(last_reason == Inverter::FaultReason::CoprocessorStatusStale);
    assert(numbers["coprocessor_status_fresh"] == 0.0f);
    assert(strings["coprocessor_fault_names"] == "unknown");
    assert(strings["coprocessor_clear_result"] == "timeout");

    SafetyFaultFrame recovered{0u, 8u, 2u, SAFETY_CLEAR_ACCEPTED};
    SafetyFaultWire_Encode(&recovered, line);
    assert(faults.consumeLine(line, 20u));
    assert(faults.readyForActuation());
    assert(faults.requestClear());
    faults.service();
    assert(strings["coprocessor_clear_result"] == "pending");
    assert(!faults.readyForActuation());
    assert(faults.consumeLine(line, 20u)); /* Same generation cannot ACK. */
    now_ms += 100u;
    faults.service();
    assert(!faults.readyForActuation());
    recovered.generation++;
    SafetyFaultWire_Encode(&recovered, line);
    assert(faults.consumeLine(line, 20u));
    assert(faults.readyForActuation());
    faults.service();
    assert(strings["coprocessor_clear_result"] == "accepted");

    recovered.state = 0u;
    SafetyFaultWire_Encode(&recovered, line);
    assert(faults.consumeLine(line, 20u));
    faults.service(true);
    assert(last_reason == Inverter::FaultReason::CoprocessorNotArmed);
    return 0;
}
