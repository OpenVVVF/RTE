#include "Inverter/Control/VoltageVectorSchedule.h"

namespace Inverter {

static VoltageVectorSchedule s_instance;

VoltageVectorSchedule& voltageVectorSchedule() {
    return s_instance;
}

bool VoltageVectorSchedule::push(const VoltageVector& v) {
    const size_t next_tail = (m_tail + 1) % CAPACITY;
    if (next_tail == m_head) {
        return false;  /* full */
    }
    m_buf[m_tail] = v;
    m_tail = next_tail;
    return true;
}

bool VoltageVectorSchedule::peek(VoltageVector& out) const {
    if (m_head == m_tail) {
        return false;
    }
    out = m_buf[m_head];
    return true;
}

bool VoltageVectorSchedule::next(VoltageVector& out) {
    if (m_head == m_tail) {
        return false;
    }
    out = m_buf[m_head];
    m_head = (m_head + 1) % CAPACITY;
    return true;
}

size_t VoltageVectorSchedule::size() const {
    const size_t h = m_head;
    const size_t t = m_tail;
    if (t >= h) {
        return t - h;
    }
    return CAPACITY - h + t;
}

void VoltageVectorSchedule::clear() {
    m_head = 0;
    m_tail = 0;
}

} // namespace Inverter
