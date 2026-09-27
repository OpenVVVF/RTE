#include "Inverter/Control/FaultManager.h"
#include "Inverter/Telemetry.h"
#include "Inverter/Drivers/GateDriver/gate_driver.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/platform_api.h"
#include "Inverter/SafetyLink.h"

#include "main.h"
#include "tim.h"

#include <cctype>
#include <cstdio>

namespace Inverter {

namespace {
uint32_t irqSave() {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    __DMB();
    return primask;
}

void irqRestore(uint32_t primask) {
    __DMB();
    __set_PRIMASK(primask);
}
} // namespace

/* Out-of-class definition for pre-C++17 ODR. */
constexpr Inverter::FaultMeta Inverter::FaultManager::s_meta[];

const char* faultReasonString(FaultReason r) {
    switch (r) {
        case FaultReason::Unspecified:          return "unspecified";
        case FaultReason::UserInjected:         return "injected by user";
        case FaultReason::DesatBreak:           return "DESAT break (/FLT low)";
        case FaultReason::GateDriverNotReady:   return "gate driver not ready";
        case FaultReason::PhaseOvercurrentSoftware: return "software overcurrent";
        case FaultReason::AdcWatchdogTrip:      return "ADC1 injected out of window";
        case FaultReason::AdcHalError:          return "ADC HAL error";
        case FaultReason::UartHalError:         return "UART error";
        case FaultReason::EncoderAmplitudeLow:  return "encoder magnitude collapsed";
        case FaultReason::EncoderAtRail:        return "encoder signal at rail";
        case FaultReason::EncoderDmaError:      return "encoder ADC DMA error";
        case FaultReason::EncoderSampleTimeout: return "no encoder sample";
        case FaultReason::CanBusOff:            return "FDCAN bus-off";
        case FaultReason::CanErrorPassive:      return "FDCAN error-passive";
        case FaultReason::CanErrorLogOverflow:  return "FDCAN error-log overflow";
        case FaultReason::CanHalError:          return "FDCAN HAL error";
        case FaultReason::VosNotReady:          return "VOS not ready";
        case FaultReason::PvdTriggered:         return "PVD triggered";
        case FaultReason::AvdTriggered:         return "AVD triggered";
        case FaultReason::Max22530CrcMismatch:  return "CRC mismatch";
        case FaultReason::Max22530SpiFrameError:return "SPI frame/CRC error";
        case FaultReason::Max22530SpiDmaError:  return "SPI DMA error";
        case FaultReason::Max22530AdcDiagnostic:return "ADC functionality diagnostic";
        case FaultReason::Max22530FieldLoss:    return "field-side data loss";
        case FaultReason::Max22530Overvoltage:  return "comparator high threshold";
        case FaultReason::Max22530Undervoltage: return "comparator low threshold";
        case FaultReason::FramInitIdMismatch:   return "device ID mismatch";
        case FaultReason::FramReadFailed:       return "read failed";
        case FaultReason::FramWriteFailed:      return "write failed";
        case FaultReason::FramCommandFailed:    return "command failed";
        case FaultReason::ThrottlePlausibilityMismatch: return "throttle A/B mismatch";
        case FaultReason::TempSensorOpenInv1:   return "inverter sensor 1 open circuit";
        case FaultReason::TempSensorOpenInv2:   return "inverter sensor 2 open circuit";
        case FaultReason::TempSensorOpenInv3:   return "inverter sensor 3 open circuit";
        case FaultReason::TempSensorOpenMot:    return "motor sensor open circuit";
        case FaultReason::TempSensorShortInv1:  return "inverter sensor 1 short circuit";
        case FaultReason::TempSensorShortInv2:  return "inverter sensor 2 short circuit";
        case FaultReason::TempSensorShortInv3:  return "inverter sensor 3 short circuit";
        case FaultReason::TempSensorShortMot:   return "motor sensor short circuit";
        case FaultReason::OvertemperatureInv1:  return "inverter sensor 1 over limit";
        case FaultReason::OvertemperatureInv2:  return "inverter sensor 2 over limit";
        case FaultReason::OvertemperatureInv3:  return "inverter sensor 3 over limit";
        case FaultReason::OvertemperatureMotor: return "motor over temperature limit";
        case FaultReason::SensorRefOutOfRange:  return "reference out of window";
        case FaultReason::OnboardOvertemperature: return "onboard temp over limit (I2C5)";
        case FaultReason::RailOvervoltage:      return "rail bus overvoltage (I2C4)";
        case FaultReason::RailUndervoltage:     return "rail bus undervoltage (I2C4)";
        case FaultReason::TorqueLossAbsent:     return "iq near zero with saturated vq request";
        case FaultReason::OverTorqueLimit:      return "iq above 110% of calibrated max torque current";
        case FaultReason::EncoderLossWhileDriving: return "encoder feedback lost while driving";
        case FaultReason::DcLinkOvervoltageWarning: return "Vbus above OV warning threshold; regen disabled";
        case FaultReason::DcLinkUndervoltageDerate: return "Vbus below UV derate threshold; current limited";
        case FaultReason::GateDriverFaultPin:   return "gate-driver /FLT asserted (GPIO poll)";
        case FaultReason::Count:                break;
    }
    return "unknown";
}

FaultManager& FaultManager::instance() {
    static FaultManager s_instance;
    return s_instance;
}

const FaultMeta* FaultManager::metaFor(FaultSource src) {
    for (size_t i = 0; i < metaCount(); ++i) {
        if (s_meta[i].source == src) {
            return &s_meta[i];
        }
    }
    return nullptr;
}

const FaultMeta* FaultManager::metaTable() {
    return s_meta;
}

FaultSource FaultManager::sourceFromName(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return FaultSource::None;
    }
    for (size_t i = 0; i < metaCount(); ++i) {
        const char* a = name;
        const char* b = s_meta[i].name;
        while (*a && *b &&
               std::tolower(static_cast<unsigned char>(*a)) ==
               std::tolower(static_cast<unsigned char>(*b))) {
            ++a; ++b;
        }
        if (*a == '\0' && *b == '\0') {
            return s_meta[i].source;
        }
    }
    return FaultSource::None;
}

void FaultManager::raise(FaultSource src, FaultReason reason) {
    if (src == FaultSource::None) {
        return;
    }

    /* Critical sources can arrive from ADC/SPI/DMA IRQs. Open all six PWM
     * switches immediately; the application loop then performs the slower
     * gate-reset and power-off actions. */
    const FaultMeta* raised_meta = metaFor(src);
    if (raised_meta != nullptr && raised_meta->severity == FaultSeverity::Critical) {
        TIM1->EGR = TIM_EGR_BG;
    }

    const uint32_t primask = irqSave();
    if (!m_active.test(src)) {
        m_active.set(src);
        m_pending_log.set(src);
        for (size_t i = 0; i < metaCount(); ++i) {
            if (s_meta[i].source == src && i < REASON_COUNT) {
                m_reason[i] = reason;
                break;
            }
        }
    }
    irqRestore(primask);
}

void FaultManager::clear(FaultSource src) {
    if (src == FaultSource::None) {
        return;
    }
    FaultBits bits = FaultBits::bit(src);
    clearMask(bits);
}

void FaultManager::clearMask(const FaultBits& mask) {
    const uint32_t primask = irqSave();
    m_active.clearFrom(mask);
    m_pending_log.clearFrom(mask);
    for (size_t i = 0; i < metaCount() && i < REASON_COUNT; ++i) {
        if (mask.test(s_meta[i].source)) {
            m_reason[i] = FaultReason::Unspecified;
        }
    }
    bool criticalRemains = false;
    for (size_t i = 0; i < metaCount(); ++i) {
        if (s_meta[i].severity == FaultSeverity::Critical &&
            m_active.test(s_meta[i].source)) {
            criticalRemains = true;
            break;
        }
    }
    if (!criticalRemains) {
        m_safety_executed = false;
    }
    irqRestore(primask);
}

void FaultManager::clearAll() {
    const uint32_t primask = irqSave();
    m_active = FaultBits{};
    m_pending_log = FaultBits{};
    m_safety_executed = false;
    for (size_t i = 0; i < REASON_COUNT; ++i) {
        m_reason[i] = FaultReason::Unspecified;
    }
    irqRestore(primask);
}

bool FaultManager::isActive() const {
    const uint32_t primask = irqSave();
    const bool active = m_active.any();
    irqRestore(primask);
    return active;
}

bool FaultManager::isActive(FaultSource src) const {
    const uint32_t primask = irqSave();
    const bool active = m_active.test(src);
    irqRestore(primask);
    return active;
}

bool FaultManager::isActiveMask(const FaultBits& mask) const {
    const uint32_t primask = irqSave();
    const bool active = m_active.intersects(mask);
    irqRestore(primask);
    return active;
}

bool FaultManager::isSeverityActive(FaultSeverity severity) const {
    const FaultBits flags = activeBits();
    if (!flags.any()) {
        return false;
    }
    for (size_t i = 0; i < metaCount(); ++i) {
        if (s_meta[i].severity == severity && flags.test(s_meta[i].source)) {
            return true;
        }
    }
    return false;
}

uint32_t FaultManager::activeFlags() const {
    const uint32_t primask = irqSave();
    const uint32_t flags = m_active.w[0];
    irqRestore(primask);
    return flags;
}

FaultBits FaultManager::activeBits() const {
    const uint32_t primask = irqSave();
    const FaultBits flags = m_active;
    irqRestore(primask);
    return flags;
}

void FaultManager::publishStatus() {
    const FaultBits flags = activeBits();

    /* Backward-compatible encoding: the familiar single 0x%08X word whenever
     * only word 0 is nonzero (identical to the pre-1024-bit wire format).
     * Nonzero higher words append as ",<index>:0x%08X" (compact form; only
     * nonzero words are listed).  The main_fault_* mirrors carry the same
     * strings so hosts can tell main-MCU and coprocessor fault reports apart. */
    char fault_flags[96] = {};
    size_t used = static_cast<size_t>(std::snprintf(
        fault_flags, sizeof(fault_flags), "0x%08lX",
        static_cast<unsigned long>(flags.w[0])));
    for (size_t i = 1; i < FaultBits::WORDS; ++i) {
        if (flags.w[i] == 0U) continue;
        const int written = std::snprintf(fault_flags + used,
                                          sizeof(fault_flags) - used,
                                          ",%lu:0x%08lX",
                                          static_cast<unsigned long>(i),
                                          static_cast<unsigned long>(flags.w[i]));
        if (written < 0 || static_cast<size_t>(written) >= sizeof(fault_flags) - used) {
            break;
        }
        used += static_cast<size_t>(written);
    }
    Telemetry::log("fault_flags_hex", fault_flags);
    Telemetry::log("main_fault_flags_hex", fault_flags);

    char fault_names[512] = {};
    size_t used_names = 0;
    for (size_t i = 0; i < metaCount(); ++i) {
        const FaultMeta& meta = s_meta[i];
        if (!flags.test(meta.source)) {
            continue;
        }
        const int written = std::snprintf(fault_names + used_names,
                                          sizeof(fault_names) - used_names,
                                          "%s%s", used_names ? "," : "", meta.name);
        if (written < 0 || static_cast<size_t>(written) >= sizeof(fault_names) - used_names) {
            break;
        }
        used_names += static_cast<size_t>(written);
    }
    Telemetry::log("fault_active_names", used_names ? fault_names : "none");
    Telemetry::log("main_fault_names", used_names ? fault_names : "none");
}

void FaultManager::printSummary() {
    const FaultBits flags = activeBits();

    if (!flags.any()) {
        Telemetry::printf("[FAULT][MAIN] none active");
        return;
    }

    for (size_t i = 0; i < metaCount(); ++i) {
        const auto& m = s_meta[i];
        if (flags.test(m.source)) {
            const char* reason = (i < REASON_COUNT) ? faultReasonString(m_reason[i])
                                                    : "unspecified";
            char sev_char = '?';
            switch (m.severity) {
                case FaultSeverity::Warning:  sev_char = 'W'; break;
                case FaultSeverity::High:     sev_char = 'H'; break;
                case FaultSeverity::Critical: sev_char = 'C'; break;
            }
            if (i < REASON_COUNT && m_reason[i] != FaultReason::Unspecified) {
                Telemetry::printf("[FAULT][MAIN][%c][%s] %s: %s (%s)",
                                  sev_char, m.category, m.name, m.description, reason);
            } else {
                Telemetry::printf("[FAULT][MAIN][%c][%s] %s: %s",
                                  sev_char, m.category, m.name, m.description);
            }
        }
    }
}

void FaultManager::service() {
    /* SG-14 backstop: poll the gate-driver /FLT pin (PC11, active-low).
     * The TIM1 BKIN hardware path (break IRQ) is the fast route to PwmBreak;
     * this catches any /FLT the hardware path misses — a BKIN wiring or
     * polarity mismatch, or an IRQ that never fired — at 100 Hz.  Guards:
     * only while the gate rail is ON and the driver is out of reset; with
     * the rail off or reset asserted, /FLT reads low legitimately. */
    static constexpr uint32_t GATE_FLT_POLL_COUNT = 3U;
    const bool gate_rail_on =
        HAL_GPIO_ReadPin(GATE_DRIVER_POWER_ENABLE_GPIO_Port,
                         GATE_DRIVER_POWER_ENABLE_Pin) == GPIO_PIN_SET;
    const bool gate_in_reset =
        HAL_GPIO_ReadPin(GATE_DRIVER_RESET_GPIO_Port,
                         GATE_DRIVER_RESET_Pin) == GPIO_PIN_RESET;
    if (gate_rail_on && !gate_in_reset && GateDriver_IsFault()) {
        if (++m_gate_flt_count >= GATE_FLT_POLL_COUNT) {
            m_gate_flt_count = 0;
            raise(FaultSource::PwmBreak, FaultReason::GateDriverFaultPin);
        }
    } else {
        m_gate_flt_count = 0;
    }

    const uint32_t primask = irqSave();
    const FaultBits pending = m_pending_log;
    m_pending_log = FaultBits{};
    irqRestore(primask);

    if (!pending.any()) {
        return;
    }

    for (size_t i = 0; i < metaCount(); ++i) {
        const auto& m = s_meta[i];
        if (pending.test(m.source)) {
            const char* reason = (i < REASON_COUNT) ? faultReasonString(m_reason[i])
                                                    : "unspecified";
            char sev_char = '?';
            switch (m.severity) {
                case FaultSeverity::Warning:  sev_char = 'W'; break;
                case FaultSeverity::High:     sev_char = 'H'; break;
                case FaultSeverity::Critical: sev_char = 'C'; break;
            }
            if (i < REASON_COUNT && m_reason[i] != FaultReason::Unspecified) {
                Telemetry::printf("[FAULT][MAIN][%c][%s] %s triggered: %s",
                                  sev_char, m.category, m.name, reason);
            } else {
                Telemetry::printf("[FAULT][MAIN][%c][%s] %s triggered",
                                  sev_char, m.category, m.name);
            }
        }
    }
}

void FaultManager::executeSafetyActions() {
    if (!isSeverityActive(FaultSeverity::Critical)) {
        /* Reset the one-shot so the next critical fault logs/shuts down again. */
        const uint32_t primask = irqSave();
        m_safety_executed = false;
        irqRestore(primask);
        return;
    }

    if (m_safety_executed) {
        return;
    }
    m_safety_executed = true;
    SafetyLink_Stop();

    /* Triple-redundant shutdown:
     * 1. Force TIM1 break -> hardware disables all PWM outputs (MOE clear).
     * 2. Assert gate-driver reset line.
     * 3. Turn off gate-driver power rail. */
    if (TIM1 != nullptr) {
        TIM1->EGR |= TIM_EGR_BG;
    }
    platform_set_control_outputs_enabled(false);
    PWM_StopSPWM();
    GateDriver_DisableOutputs();
    GateDriver_EnablePower(false);

    Telemetry::printf("[SAFETY] Critical fault -> PWM break, gate driver reset, power off");
}

void FaultManager::testFault(FaultSource src, FaultReason reason) {
    raise(src, reason);
}

} // namespace Inverter
