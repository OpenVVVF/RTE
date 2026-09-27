#include "Inverter/Control/CoprocessorFaults.h"

#include "Inverter/Telemetry.h"
#include "main.h"

#include <cstdio>
#include <cstring>

namespace Inverter {

namespace {

struct FaultName { uint32_t bit; const char* name; };
constexpr FaultName NAMES[] = {
    {1u << 0, "GateDriver"},
    {1u << 1, "PowerStuckOn"},
    {1u << 2, "PowerFeedbackLost"},
    {1u << 4, "MainHeartbeatLost"},
    {1u << 5, "Internal"}
};

const char* stateName(uint8_t state) {
    switch (state) {
        case 0: return "INHIBITED";
        case 1: return "POWERING";
        case 2: return "ARMED";
        case 3: return "FAULT_LATCHED";
        default: return "INVALID";
    }
}

const char* clearName(uint8_t result) {
    switch (result) {
        case SAFETY_CLEAR_NONE: return "none";
        case SAFETY_CLEAR_ACCEPTED: return "accepted";
        case SAFETY_CLEAR_REFUSED: return "refused";
        case SAFETY_CLEAR_NO_FAULT: return "no_fault";
        default: return "invalid";
    }
}

void formatNames(uint32_t flags, char* text, size_t capacity) {
    size_t used = 0;
    text[0] = '\0';
    for (const auto& fault : NAMES) {
        if ((flags & fault.bit) == 0u) continue;
        const int written = std::snprintf(text + used, capacity - used,
                                          "%s%s", used ? "," : "", fault.name);
        if (written < 0 || static_cast<size_t>(written) >= capacity - used) break;
        used += static_cast<size_t>(written);
    }
    if (used == 0) std::snprintf(text, capacity, "%s", flags ? "UnknownBit" : "none");
}

} // namespace

CoprocessorFaults& CoprocessorFaults::instance() {
    static CoprocessorFaults faults;
    return faults;
}

bool CoprocessorFaults::consumeLine(const char* line, size_t length) {
    if (line == nullptr || length < 4u || std::memcmp(line, "!SF1", 4u) != 0) {
        return false;
    }
    SafetyFaultFrame frame{};
    if (!SafetyFaultWire_Decode(line, length, &frame)) {
        return true; /* Invalid internal frame must not execute as a command. */
    }
    const bool changed = !m_seen || frame.generation != m_frame.generation ||
                         frame.faults != m_frame.faults ||
                         frame.clear_result != m_frame.clear_result;
    m_frame = frame;
    m_last_rx_ms = HAL_GetTick();
    m_seen = true;
    m_publish_due = true;
    if (changed && (frame.faults != 0u || frame.clear_result != SAFETY_CLEAR_NONE)) {
        char names[128];
        formatNames(frame.faults, names, sizeof(names));
        Telemetry::printf("[FAULT][COPROCESSOR] state=%s faults=%s clear=%s",
                          stateName(frame.state), names, clearName(frame.clear_result));
    }
    return true;
}

bool CoprocessorFaults::fresh() const {
    return m_seen && (uint32_t)(HAL_GetTick() - m_last_rx_ms) <= 1500u;
}

bool CoprocessorFaults::requestClear() {
    if (m_clear_active || !fresh()) return false;
    m_clear_active = true;
    m_clear_started_ms = HAL_GetTick();
    HAL_GPIO_WritePin(COPROCESSOR_WAKEUP_GPIO_Port, COPROCESSOR_WAKEUP_Pin,
                      GPIO_PIN_SET);
    return true;
}

void CoprocessorFaults::service() {
    const uint32_t now_ms = HAL_GetTick();
    if (m_clear_active && (uint32_t)(now_ms - m_clear_started_ms) >= 100u) {
        HAL_GPIO_WritePin(COPROCESSOR_WAKEUP_GPIO_Port, COPROCESSOR_WAKEUP_Pin,
                          GPIO_PIN_RESET);
        m_clear_active = false;
    }
    if (m_publish_due || (uint32_t)(now_ms - m_last_publish_ms) >= 1000u) {
        publishStatus();
        m_last_publish_ms = now_ms;
        m_publish_due = false;
    }
}

void CoprocessorFaults::publishStatus() const {
    char flags[16];
    char names[128];
    std::snprintf(flags, sizeof(flags), "0x%08lX",
                  static_cast<unsigned long>(m_seen ? m_frame.faults : 0u));
    formatNames(m_seen ? m_frame.faults : 0u, names, sizeof(names));
    Telemetry::log("coprocessor_fault_flags_hex", m_seen ? flags : "unknown");
    Telemetry::log("coprocessor_fault_names", m_seen ? names : "unknown");
    Telemetry::log("coprocessor_safety_state",
                   fresh() ? stateName(m_frame.state) : "STALE");
    Telemetry::log("coprocessor_clear_result",
                   m_seen ? clearName(m_frame.clear_result) : "unknown");
    Telemetry::log("coprocessor_status_age_ms",
                   m_seen ? static_cast<float>(HAL_GetTick() - m_last_rx_ms) : -1.0f);
}

void CoprocessorFaults::printStatus() const {
    if (!m_seen) {
        Telemetry::printf("[FAULT][COPROCESSOR] status unknown (no report received)");
        return;
    }
    const uint32_t age_ms = HAL_GetTick() - m_last_rx_ms;
    Telemetry::printf("[FAULT][COPROCESSOR] state=%s flags=0x%08lX age=%lu ms generation=%u clear=%s%s",
                      stateName(m_frame.state), static_cast<unsigned long>(m_frame.faults),
                      static_cast<unsigned long>(age_ms), m_frame.generation,
                      clearName(m_frame.clear_result), fresh() ? "" : " STALE");
    if (m_frame.faults == 0u) {
        Telemetry::printf("[FAULT][COPROCESSOR] none active");
    } else {
        for (const auto& fault : NAMES) {
            if ((m_frame.faults & fault.bit) != 0u)
                Telemetry::printf("[FAULT][COPROCESSOR] %s", fault.name);
        }
    }
}

} // namespace Inverter
