#pragma once

#include "safety_fault_wire.h"

#include <cstddef>
#include <cstdint>

namespace Inverter {

/** Latest G474 snapshot over USART3. The G474 owns its latch and PWR2;
 * the H7 also refuses actuation without a fresh, fault-free ARMED report. */
class CoprocessorFaults {
public:
    static CoprocessorFaults& instance();

    /** Consume a private status line before it reaches the user command
     * parser. Returns true for all !SF1 lines, including malformed ones. */
    bool consumeLine(const char* line, size_t length);
    void service(bool actuating = false);
    void printStatus() const;
    bool requestClear();
    bool fresh() const;
    bool readyForActuation() const;
    bool checkReadyForActuation() const;
    bool hasFault() const { return m_seen && m_frame.faults != 0u; }
    uint16_t generation() const { return m_frame.generation; }

private:
    CoprocessorFaults() = default;
    void publishStatus() const;

    SafetyFaultFrame m_frame{};
    uint32_t m_last_rx_ms = 0;
    uint32_t m_last_publish_ms = 0;
    uint32_t m_clear_started_ms = 0;
    bool m_seen = false;
    bool m_clear_active = false;
    bool m_clear_pending = false;
    bool m_clear_timed_out = false;
    uint16_t m_clear_generation = 0;
    bool m_publish_due = true;
    bool m_published_fresh = false;
};

} // namespace Inverter
