#include "Inverter/Drivers/I2C/RailMonitor.h"

namespace Inverter {

const char* RailMonitor::partName() const {
    switch (m_part) {
        case Part::INA226:  return "INA226";
        case Part::INA228:  return "INA228";
        case Part::INA3221: return "INA3221";
        default:            return "none";
    }
}

/* ---------------- INA226 math ---------------- */

float RailMonitor::ina226CurrentLsb(float max_expected_a) {
    return max_expected_a / 32768.0f;
}

uint16_t RailMonitor::ina226CalValue(float current_lsb_a, float shunt_ohm) {
    /* CAL = 0.00512 / (Current_LSB * RSHUNT), rounded to the nearest 16-bit
     * register value (quantisation error is +/-1 LSB either way). */
    const float denom = current_lsb_a * shunt_ohm;
    if (denom <= 0.0f) {
        return 0;
    }
    long cal = lround(0.00512 / static_cast<double>(denom));
    if (cal > 0x7FFF) {  /* CAL bit15 is reserved; clamp rather than wrap. */
        cal = 0x7FFF;
    } else if (cal < 1) {
        cal = 1;
    }
    return static_cast<uint16_t>(cal);
}

float RailMonitor::ina226CurrentFromRaw(int16_t raw, float current_lsb_a) {
    return static_cast<float>(raw) * current_lsb_a;
}

float RailMonitor::ina226PowerFromRaw(uint16_t raw, float current_lsb_a) {
    return static_cast<float>(raw) * 25.0f * current_lsb_a;
}

/* ---------------- INA228 math ---------------- */

float RailMonitor::ina228CurrentLsb(float max_expected_a) {
    return max_expected_a / 524288.0f;  /* 2^19, signed 20-bit positive range */
}

uint16_t RailMonitor::ina228ShuntCalValue(float current_lsb_a, float shunt_ohm) {
    /* SHUNT_CAL = 13107.2e6 * Current_LSB * RSHUNT  (ADCRANGE = 0), rounded
     * to the nearest register value.  The double intermediate keeps the
     * exact mathematical result (e.g. 10 A / 5 mOhm = exactly 1250) from
     * being truncated to 1249 by float rounding of the inputs. */
    const double cal = 13107.2e6 * static_cast<double>(current_lsb_a) *
                       static_cast<double>(shunt_ohm);
    if (cal < 1.0) {
        return 0;
    }
    if (cal > 0xFFFF) {
        return 0xFFFF;
    }
    return static_cast<uint16_t>(lround(cal));
}

int32_t RailMonitor::signExtend24(uint32_t raw24) {
    raw24 &= 0x00FFFFFFu;
    return (raw24 & 0x00800000u) != 0u
               ? static_cast<int32_t>(raw24 | 0xFF000000u)
               : static_cast<int32_t>(raw24);
}

float RailMonitor::ina228BusVFromRaw(uint32_t raw24) {
    /* 20-bit result left-aligned in the 24-bit register. */
    const int32_t code = signExtend24(raw24) >> 4;
    return static_cast<float>(code) * 195.3125e-6f;
}

float RailMonitor::ina228CurrentFromRaw(uint32_t raw24, float current_lsb_a) {
    const int32_t code = signExtend24(raw24) >> 4;
    return static_cast<float>(code) * current_lsb_a;
}

float RailMonitor::ina228PowerFromRaw(uint32_t raw24, float current_lsb_a) {
    /* POWER is a full 24-bit register (bits 23:0) on the INA228 — unlike
     * VBUS/CURRENT, which hold 20-bit data in bits 23:4.  Do not right-shift;
     * a 20-bit decode would under-report power 16x.  (SLYS021 §7.6.1.9.) */
    const uint32_t code = raw24 & 0x00FFFFFFu;
    return static_cast<float>(code) * 3.2f * current_lsb_a;
}

float RailMonitor::ina228DieTempCFromRaw(uint16_t raw) {
    return static_cast<float>(static_cast<int16_t>(raw)) * 7.8125e-3f;
}

/* ---------------- INA3221 math ---------------- */

float RailMonitor::ina3221CurrentA(float shunt_v, float shunt_ohm) {
    return (shunt_ohm > 0.0f) ? (shunt_v / shunt_ohm) : 0.0f;
}

/* ---------------- bus primitives ---------------- */

bool RailMonitor::readReg16(uint8_t reg, uint16_t& value,
                            uint32_t timeout_ms) const {
    uint8_t buf[2] = {0, 0};
    if (!m_bus->transfer(m_addr, &reg, 1, buf, 2, timeout_ms)) {
        return false;
    }
    value = static_cast<uint16_t>((static_cast<uint16_t>(buf[0]) << 8) | buf[1]);
    return true;
}

bool RailMonitor::writeReg16(uint8_t reg, uint16_t value,
                             uint32_t timeout_ms) const {
    const uint8_t buf[3] = {
        reg,
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value & 0xFFu),
    };
    return m_bus->transfer(m_addr, buf, 3, nullptr, 0, timeout_ms);
}

bool RailMonitor::readReg24(uint8_t reg, uint32_t& value,
                            uint32_t timeout_ms) const {
    uint8_t buf[3] = {0, 0, 0};
    if (!m_bus->transfer(m_addr, &reg, 1, buf, 3, timeout_ms)) {
        return false;
    }
    value = (static_cast<uint32_t>(buf[0]) << 16) |
            (static_cast<uint32_t>(buf[1]) << 8) |
             static_cast<uint32_t>(buf[2]);
    return true;
}

/* ---------------- init / poll ---------------- */

bool RailMonitor::init(II2cBus& bus, uint8_t addr7, const Config& cfg,
                       uint32_t timeout_ms) {
    m_bus = &bus;
    m_addr = addr7;
    m_part = Part::None;
    m_cal = 0;
    m_current_lsb = 0.0f;
    m_shunt_ohm = cfg.shunt_ohm;
    m_last_mfg = 0;
    m_last_dev = 0;

    if (cfg.shunt_ohm <= 0.0f || cfg.max_expected_a <= 0.0f) {
        return false;  /* cannot calibrate without a sane shunt model */
    }
    if (!bus.isReady(addr7, timeout_ms)) {
        return false;
    }

    /* Capability table, tried in register-collision-safe order:
     * 1) INA226/INA3221 style ID registers 0xFE/0xFF.
     * 2) INA228 style ID registers 0x3E/0x3F (only when 1) fails cleanly,
     *    since on an INA226/3221 those addresses read as 0. */
    uint16_t mfg = 0, dev = 0;
    bool legacy_ok = readReg16(REG_INA_MFG_ID, mfg, timeout_ms) &&
                     readReg16(REG_INA_DIE_ID, dev, timeout_ms);
    if (legacy_ok && mfg == TI_MFG_ID) {
        m_last_mfg = mfg;
        m_last_dev = dev;
        /* Die-ID registers are DID[15:4] + RID[3:0] (revision): US-fab INA226
         * revs report 0x2261, and treating the nibble as part of the ID
         * rejects legitimate parts.  Mask the RID nibble (SBOS547 Table 7-1
         * note 3 / Table 7-15; INA3221 ID register has the same layout). */
        if ((dev & 0xFFF0u) == INA226_DIE_ID) {
            m_part = Part::INA226;
            m_current_lsb = ina226CurrentLsb(cfg.max_expected_a);
            m_cal = ina226CalValue(m_current_lsb, cfg.shunt_ohm);
            /* Continuous shunt+bus, 1.1 ms conversions (POR default).  The
             * CAL write is what makes the current/power registers valid.
             * RMW the MODE bits too: a warm boot could leave a stale
             * triggered/shutdown mode behind (POR CONFIG = 0x4127). */
            uint16_t config = 0;
            if (!writeReg16(REG_INA226_CAL, m_cal, timeout_ms)) {
                return false;
            }
            if (!readReg16(REG_INA226_CONFIG, config, timeout_ms)) {
                return false;
            }
            if ((config & INA226_CFG_MODE_CONT) != INA226_CFG_MODE_CONT) {
                config = static_cast<uint16_t>((config & ~INA226_CFG_MODE_CONT) |
                                               INA226_CFG_MODE_CONT);
                return writeReg16(REG_INA226_CONFIG, config, timeout_ms);
            }
            return true;
        }
        if ((dev & 0xFFF0u) == INA3221_DIE_ID) {
            m_part = Part::INA3221;
            /* RMW: enable all three channels (kernel programs CH_EN; disabled
             * channels hold stale data that must never read back as valid)
             * and force continuous shunt+bus mode after warm boots. */
            uint16_t config = 0;
            if (!readReg16(REG_INA226_CONFIG, config, timeout_ms)) {
                return false;
            }
            const uint16_t want = static_cast<uint16_t>(
                (config | INA3221_CFG_CH_EN | INA226_CFG_MODE_CONT));
            if (want != config) {
                return writeReg16(REG_INA226_CONFIG, want, timeout_ms);
            }
            return true;
        }
        return false;  /* known TI family but unsupported die */
    }

    mfg = 0; dev = 0;
    const bool got228 = readReg16(REG_INA228_MFG_ID, mfg, timeout_ms) &&
                        readReg16(REG_INA228_DEV_ID, dev, timeout_ms);
    if (got228) {
        /* Record the raw IDs regardless of match so a rejected probe stays
         * diagnosable via lastMfgId()/lastDevId() (same contract as the
         * legacy-ID branch above). */
        m_last_mfg = mfg;
        m_last_dev = dev;
    }
    if (got228 && mfg == TI_MFG_ID && (dev & 0xFFF0u) == INA228_DEV_ID_HI) {
        m_part = Part::INA228;
        m_current_lsb = ina228CurrentLsb(cfg.max_expected_a);
        m_cal = ina228ShuntCalValue(m_current_lsb, cfg.shunt_ohm);
        /* POR defaults keep ADCRANGE=0 (+/-163.84 mV shunt swing) and
         * continuous conversion of bus/shunt/temp, but a warm boot could
         * leave ADCRANGE=1 behind (4x finer shunt LSB -> currents would
         * read 4x high under the ADCRANGE=0 math).  RMW CONFIG to force
         * ADCRANGE=0; everything else is left at the running config. */
        uint16_t config228 = 0;
        if (m_cal == 0 ||
            !writeReg16(REG_INA228_SHUNT_CAL, m_cal, timeout_ms) ||
            !readReg16(REG_INA228_CONFIG, config228, timeout_ms)) {
            return false;
        }
        if ((config228 & INA228_CFG_ADCRANGE) != 0u) {
            config228 = static_cast<uint16_t>(config228 & ~INA228_CFG_ADCRANGE);
            return writeReg16(REG_INA228_CONFIG, config228, timeout_ms);
        }
        return true;
    }
    return false;
}

bool RailMonitor::poll(uint8_t channel, Sample& out,
                       uint32_t timeout_ms) const {
    if ((m_bus == nullptr) || (m_part == Part::None)) {
        return false;
    }
    if (channel >= channels()) {
        return false;
    }

    switch (m_part) {
        case Part::INA226: {
            uint16_t bus = 0, power = 0;
            int16_t current = 0;
            uint16_t cur_raw = 0;
            if (!readReg16(REG_INA226_BUS, bus, timeout_ms) ||
                !readReg16(REG_INA226_CURRENT, cur_raw, timeout_ms) ||
                !readReg16(REG_INA226_POWER, power, timeout_ms)) {
                return false;
            }
            current = static_cast<int16_t>(cur_raw);
            /* A raw current at the rails means the chip's internal
             * shunt*CAL math may have overflowed (the count wraps to the
             * opposite sign when |current| exceeds the calibrated
             * full-scale).  Confirm via the Mask/Enable math-overflow flag
             * before trusting a near-full-scale sample; the OVF read only
             * costs one transaction at the rails. */
            if (current >= 32760 || current <= -32768) {
                uint16_t mask_en = 0;
                if (readReg16(REG_INA226_MASK_EN, mask_en, timeout_ms) &&
                    (mask_en & INA226_OVF) != 0u) {
                    return false;  /* math overflow: sample is a wrapped rail */
                }
            }
            out.bus_v     = ina226BusVFromRaw(bus);
            out.current_a = ina226CurrentFromRaw(current, m_current_lsb);
            out.power_w   = ina226PowerFromRaw(power, m_current_lsb);
            return true;
        }
        case Part::INA228: {
            uint32_t bus = 0, current = 0, power = 0;
            if (!readReg24(REG_INA228_VBUS, bus, timeout_ms) ||
                !readReg24(REG_INA228_CURRENT, current, timeout_ms) ||
                !readReg24(REG_INA228_POWER, power, timeout_ms)) {
                return false;
            }
            out.bus_v     = ina228BusVFromRaw(bus);
            out.current_a = ina228CurrentFromRaw(current, m_current_lsb);
            out.power_w   = ina228PowerFromRaw(power, m_current_lsb);
            return true;
        }
        case Part::INA3221: {
            /* Ch n (1-based in the datasheet): shunt 0x01+2n, bus 0x02+2n. */
            const uint8_t reg_shunt = static_cast<uint8_t>(0x01 + 2 * channel);
            const uint8_t reg_bus   = static_cast<uint8_t>(0x02 + 2 * channel);
            uint16_t shunt_raw = 0, bus_raw = 0;
            if (!readReg16(reg_bus, bus_raw, timeout_ms) ||
                !readReg16(reg_shunt, shunt_raw, timeout_ms)) {
                return false;
            }
            const float shunt_v =
                ina3221ShuntVFromRaw(static_cast<int16_t>(shunt_raw));
            out.bus_v     = ina3221BusVFromRaw(static_cast<uint16_t>(bus_raw));
            out.current_a = ina3221CurrentA(shunt_v, m_shunt_ohm);
            out.power_w   = out.bus_v * out.current_a;  /* no power reg; derive */
            return true;
        }
        default:
            return false;
    }
}

} // namespace Inverter
