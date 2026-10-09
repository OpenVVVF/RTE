#include "Inverter/Control/FaultManager.h"
#include "Inverter/Control/CoprocessorFaults.h"
#include "Inverter/Control/ControlSupervisor.h"
#include "Inverter/Control/FocControlManager.h"
#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Calibration/Common/CalibrationHardware.h"
#include "Inverter/SafetyLink.h"
#include "main.h"

#include <cassert>
#include <map>
#include <string>

GPIO_TypeDef test_gpio_d = {1};
GPIO_TypeDef test_gpio_c = {2};
TIM_TypeDef test_tim1 = {0, 0, 0};
TIM_TypeDef *test_tim1_instance = &test_tim1;
static uint32_t now_ms;
static GPIO_PinState power_level = GPIO_PIN_SET;
static GPIO_PinState reset_level = GPIO_PIN_SET;
static bool graph_enabled = true;
static bool driver_ready = true;
static unsigned release_count;
static std::map<std::string, std::string> strings;

uint32_t HAL_GetTick() { return now_ms; }
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef*, uint16_t pin) {
    return pin == GATE_DRIVER_POWER_ENABLE_Pin ? power_level : reset_level;
}
void HAL_GPIO_WritePin(GPIO_TypeDef*, uint16_t pin, GPIO_PinState level) {
    if (pin == GATE_DRIVER_POWER_ENABLE_Pin) power_level = level;
    if (pin == GATE_DRIVER_RESET_Pin) {
        reset_level = level;
        if (level == GPIO_PIN_SET) ++release_count;
    }
}
void GateDriver_DisableOutputs() { reset_level = GPIO_PIN_RESET; }
void GateDriver_EnablePower(bool enabled) { power_level = enabled ? GPIO_PIN_SET : GPIO_PIN_RESET; }
bool GateDriver_IsReady() { return driver_ready; }
bool GateDriver_IsFault() { return false; }
void PWM_StopSPWM() {}
void PWM_SetThreePhaseDuty(float, float, float) {}
void platform_set_control_outputs_enabled(bool enabled) { graph_enabled = enabled; }
namespace Telemetry {
void log(const char* key, const char* value) { strings[key] = value; }
void log(const char*, float) {}
void printf(const char*, ...) {}
}
namespace Inverter {
ControlSupervisor& ControlSupervisor::instance() { static ControlSupervisor s; return s; }
FocControlManager& focControlManager() { static FocControlManager s; return s; }
OpenLoopController& openLoopController() { static OpenLoopController s; return s; }
}

static void receive(const SafetyFaultFrame& frame) {
    char line[SAFETY_FAULT_WIRE_LENGTH];
    SafetyFaultWire_Encode(&frame, line);
    assert(Inverter::CoprocessorFaults::instance().consumeLine(line, 20u));
}

int main() {
    using namespace Inverter;
    auto& faults = FaultManager::instance();
    auto& coprocessor = CoprocessorFaults::instance();
    SafetyLink_Init();
    receive({0u, 1u, 2u, SAFETY_CLEAR_NONE});
    assert(coprocessor.readyForActuation());

    // A G474 trip shuts down local outputs and lifecycle flags immediately.
    focControlManager().running = true; // Includes a pending startup in firmware.
    openLoopController().running = true;
    receive({1u << 2, 2u, 3u, SAFETY_CLEAR_NONE});
    coprocessor.service(true);
    assert(faults.isActive(FaultSource::CoprocessorSafety));
    faults.executeSafetyActions();
    assert(!graph_enabled && !SafetyLink_IsEnabled());
    assert(power_level == GPIO_PIN_RESET && reset_level == GPIO_PIN_RESET);
    assert(!focControlManager().isRunning() && !openLoopController().isRunning());
    assert(ControlSupervisor::instance().faulted);
    assert(strings["main_fault_flags_hex"] == "0x00000000,1:0x00000040");
    assert(strings["main_fault_names"] == "CoprocessorSafety");

    // Clearing a warning or one of two Critical faults cannot restart PD8.
    faults.raise(FaultSource::PhaseOvercurrent);
    faults.raise(FaultSource::UartError);
    faults.clear(FaultSource::UartError);
    assert(!SafetyLink_IsEnabled());
    faults.clear(FaultSource::CoprocessorSafety);
    assert(!SafetyLink_IsEnabled());
    faults.clear(FaultSource::PhaseOvercurrent);
    assert(SafetyLink_IsEnabled());
    assert(power_level == GPIO_PIN_RESET && reset_level == GPIO_PIN_RESET);
    assert(!graph_enabled); // Resuming monitoring does not restart the motor.

    // A stale private link trips only during actuation, including direct PWM.
    receive({0u, 3u, 2u, SAFETY_CLEAR_NONE});
    now_ms = 1501u;
    coprocessor.service(false);
    assert(!faults.isActive());
    coprocessor.service(true);
    faults.executeSafetyActions();
    assert(faults.isActive(FaultSource::CoprocessorSafety));
    assert(!SafetyLink_IsEnabled());
    faults.clearAll();
    assert(SafetyLink_IsEnabled());

    // A Critical fault during calibration warmup must never release /RST.
    receive({0u, 4u, 2u, SAFETY_CLEAR_NONE});
    CalibrationHardware hardware;
    hardware.begin();
    assert(hardware.state() == CalibrationHardware::State::POWER_STABILIZE);
    const auto before = release_count;
    faults.raise(FaultSource::PhaseOvercurrent);
    now_ms += 60u;
    hardware.update();
    assert(hardware.hasFailed());
    assert(release_count == before && reset_level == GPIO_PIN_RESET);

    // The same guard covers a coprocessor trip before calibration is ready.
    faults.clearAll();
    receive({0u, 5u, 2u, SAFETY_CLEAR_NONE});
    hardware.begin();
    receive({1u, 6u, 3u, SAFETY_CLEAR_NONE});
    now_ms += 60u;
    hardware.update();
    assert(hardware.hasFailed());
    assert(release_count == before);
    return 0;
}
