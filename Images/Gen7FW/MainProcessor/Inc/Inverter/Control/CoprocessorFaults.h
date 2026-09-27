#pragma once

#include "safety_fault_wire.h"

#include <cstddef>
#include <cstdint>

namespace Inverter {

/** Latest G474 fault snapshot received over the bridged USART3 link. The
 * snapshot is diagnostic; the G474 remains the authority for its own latch
 * and gate-power switch. */
class CoprocessorFaults {
public:
    static CoprocessorFaults& instance();

    /** Consume a private status line before it reaches the user command
     * parser. Returns true for all !SF1 lines, including malformed ones. */
    bool consumeLine(const char* line, size_t length);
    void service();
    void printStatus() const;
    bool requestClear();
    bool fresh() const;
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
    bool m_publish_due = true;
};

} // namespace Inverter
