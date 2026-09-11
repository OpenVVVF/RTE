#include "Inverter/Drivers/I2C/OnboardTempSensor.h"

#include <cmath>

namespace Inverter {

const char* OnboardTempSensor::partName() const {
    switch (m_part) {
        case Part::Tmp1075: return "TMP1075";
        case Part::Generic: return "TMP102/PCT2075-family";
        default:            return "none";
    }
}

float OnboardTempSensor::decodeTempC(uint16_t reg, bool extended) {
    /* Arithmetic right shift of the signed 16-bit word: the temperature code
     * is left-aligned, so the sign bit propagates correctly in both modes. */
    const int16_t s = static_cast<int16_t>(reg);
    const int16_t code = extended ? static_cast<int16_t>(s >> 3)
                                  : static_cast<int16_t>(s >> 4);
    return static_cast<float>(code) * LSB_C;
}

uint16_t OnboardTempSensor::encodeTempReg(float temp_c, bool extended) {
    const long code = lroundf(temp_c / LSB_C);
    const int shift = extended ? 3 : 4;
    return static_cast<uint16_t>(static_cast<int16_t>(
        static_cast<int16_t>(code) << shift));
}

bool OnboardTempSensor::readReg(uint8_t reg, uint16_t& value,
                                uint32_t timeout_ms) const {
    uint8_t buf[2] = {0, 0};
    if (!m_bus->transfer(m_addr, &reg, 1, buf, 2, timeout_ms)) {
        return false;
    }
    value = static_cast<uint16_t>((static_cast<uint16_t>(buf[0]) << 8) | buf[1]);
    return true;
}

bool OnboardTempSensor::writeReg(uint8_t reg, uint16_t value,
                                 uint32_t timeout_ms) const {
    const uint8_t buf[3] = {
        reg,
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value & 0xFFu),
    };
    return m_bus->transfer(m_addr, buf, 3, nullptr, 0, timeout_ms);
}

bool OnboardTempSensor::init(II2cBus& bus, uint8_t addr7, uint32_t timeout_ms) {
    m_bus  = &bus;
    m_addr = addr7;
    m_part = Part::None;
    m_extended = false;

    if (!bus.isReady(addr7, timeout_ms)) {
        return false;
    }

    /* Part identification via the DEVICE_ID register.  TMP1075 answers
     * 0x7500; TMP102 and PCT2075 have no ID register and return 0x0000 (or
     * the read fails), which we treat as the generic family member — the
     * temperature decode is identical either way. */
    uint16_t id = 0;
    if (readReg(REG_ID, id, timeout_ms) && (id == TMP1075_DEVICE_ID)) {
        m_part = Part::Tmp1075;
    } else {
        m_part = Part::Generic;
    }

    /* Latch the EM bit so poll() decodes the right code width. */
    uint16_t cfg = 0;
    if (readReg(REG_CONFIG, cfg, timeout_ms)) {
        m_extended = configIsExtended(cfg);
    }

    return true;
}

bool OnboardTempSensor::poll(float& temp_c, uint32_t timeout_ms) const {
    if ((m_bus == nullptr) || (m_part == Part::None)) {
        return false;
    }
    uint16_t raw = 0;
    if (!readReg(REG_TEMP, raw, timeout_ms)) {
        return false;
    }
    temp_c = decodeTempC(raw, m_extended);
    return true;
}

} // namespace Inverter
