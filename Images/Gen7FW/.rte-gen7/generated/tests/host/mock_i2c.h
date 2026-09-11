#pragma once

/**
 * @brief Mock I2C master + slave device register models for host-side driver
 *        verification (no MCU, no HAL — substitute for real Gen7 hardware).
 *
 * The models implement the register-visible behavior of the candidate parts:
 *   - Tmp1075Model:  TMP102/TMP1075/PCT2075 pointer-register family.
 *     TMP1075 DEVICE_ID (0x0F) = 0x7500.  TMP102/PCT2075 have no ID register;
 *     the generic model returns 0x0000 there (best-known assumption: the
 *     unimplemented pointer reads back zero).
 *   - Ina226Model:   16-bit part, MFG_ID(0xFE)=0x5449, DIE_ID(0xFF)=0x2260.
 *     Calibration transfer implemented per INA226 datasheet:
 *       CURRENT = (SHUNT * CAL) / 2048,  POWER = (CURRENT * BUS) / 20000.
 *   - Ina3221Model:  MFG_ID(0xFE)=0x5449, DIE_ID(0xFF)=0x3220, 3 channels,
 *     8 mV / 40 uV LSB in bits 15:3.  No CAL (current computed by firmware).
 *   - Ina228Model:   20-bit ADC in 24-bit registers, MFG_ID(0x3E)=0x5449,
 *     DEVICE_ID(0x3F)=0x2280 (id in bits 15:4 per Adafruit INA228 driver).
 *     Registers are filled from physics (Vshunt/Rshunt), NOT from SHUNT_CAL —
 *     the driver must return the physical V/I/P regardless of the CAL value,
 *     while the CAL value itself is asserted separately.
 */

#include "Inverter/Drivers/I2C/I2cBus.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>

namespace hosttest {

class MockDevice {
public:
    virtual ~MockDevice() = default;
    /** Handle write phase; data[0] is the register pointer. false = NACK. */
    virtual bool onWrite(const uint8_t* data, uint8_t len) = 0;
    /** Fill the read phase buffer at the current pointer. */
    virtual void onRead(uint8_t* out, uint8_t len) = 0;
};

class MockI2cBus : public Inverter::II2cBus {
public:
    void attach(uint8_t addr7, MockDevice* dev) { m_devices[addr7] = dev; }
    void detach(uint8_t addr7) { m_devices.erase(addr7); }
    uint32_t transferCount() const { return m_transfers; }

    bool transfer(uint8_t addr7,
                  const uint8_t* tx, uint8_t tx_len,
                  uint8_t* rx, uint8_t rx_len,
                  uint32_t /*timeout_ms*/) override {
        ++m_transfers;
        auto it = m_devices.find(addr7);
        if (it == m_devices.end()) {
            return false;  /* NACK: nothing at this address */
        }
        if (tx != nullptr && tx_len > 0 && !it->second->onWrite(tx, tx_len)) {
            return false;
        }
        if (rx != nullptr && rx_len > 0) {
            it->second->onRead(rx, rx_len);
        }
        return true;
    }

    bool isReady(uint8_t addr7, uint32_t /*timeout_ms*/) override {
        return m_devices.count(addr7) != 0;
    }

private:
    std::map<uint8_t, MockDevice*> m_devices;
    uint32_t m_transfers = 0;
};

/* ---------------- TMP102/TMP1075/PCT2075 ---------------- */

class Tmp1075Model : public MockDevice {
public:
    /**
     * @param has_id  true: DEVICE_ID 0x0F = 0x7500 (TMP1075).
     *                false: reads 0x0000 (TMP102 / PCT2075 assumption).
     */
    explicit Tmp1075Model(bool has_id) {
        m_regs[0x00] = 0x1900;  /* 25.0 degC */
        m_regs[0x01] = 0x0000;  /* config: EM=0 -> 12-bit */
        m_regs[0x02] = 0x4B00;  /* Tlow 75 C */
        m_regs[0x03] = 0x5000;  /* Thigh 80 C */
        m_regs[0x0F] = has_id ? 0x7500u : 0x0000u;
    }

    void setTempRaw16(uint16_t reg) { m_regs[0x00] = reg; }
    void setConfig(uint16_t cfg)    { m_regs[0x01] = cfg; }
    uint16_t config() const         { return m_regs.at(0x01); }

    bool onWrite(const uint8_t* data, uint8_t len) override {
        m_pointer = data[0];
        if (len >= 3) {
            m_regs[m_pointer] =
                static_cast<uint16_t>((data[1] << 8) | data[2]);
        }
        return true;
    }

    void onRead(uint8_t* out, uint8_t len) override {
        const uint16_t v = m_regs.count(m_pointer) ? m_regs[m_pointer] : 0;
        if (len >= 1) out[0] = static_cast<uint8_t>(v >> 8);
        if (len >= 2) out[1] = static_cast<uint8_t>(v & 0xFF);
        for (uint8_t i = 2; i < len; ++i) out[i] = 0;
    }

private:
    std::map<uint8_t, uint16_t> m_regs;
    uint8_t m_pointer = 0;
};

/* ---------------- INA226 ---------------- */

class Ina226Model : public MockDevice {
public:
    Ina226Model() {
        m_regs[0x00] = 0x4127;  /* config POR default */
        m_regs[0xFE] = 0x5449;  /* "TI" */
        m_regs[0xFF] = 0x2260;  /* die id INA226 */
    }

    /** Physical stimulus; the chip-computed current/power registers update
     * whenever CAL or the stimulus changes (datasheet transfer function). */
    void setMeasurement(float bus_v, float shunt_v) {
        const long shunt_raw = lround(shunt_v / 2.5e-6);
        const long bus_raw   = lround(bus_v / 1.25e-3);
        m_shunt_raw = static_cast<uint16_t>(static_cast<int16_t>(shunt_raw));
        m_bus_raw   = static_cast<uint16_t>(bus_raw);
        recompute();
    }

    uint16_t cal() const { return m_regs.at(0x05); }

    bool onWrite(const uint8_t* data, uint8_t len) override {
        m_pointer = data[0];
        if (len >= 3) {
            switch (m_pointer) {
                case 0x00: case 0x05: case 0x06: case 0x07:
                    m_regs[m_pointer] =
                        static_cast<uint16_t>((data[1] << 8) | data[2]);
                    break;
                default:
                    break;  /* result/ID regs are read-only */
            }
            if (m_pointer == 0x05) {
                recompute();
            }
        }
        return true;
    }

    void onRead(uint8_t* out, uint8_t len) override {
        uint16_t v = 0;
        if (m_pointer == 0x01)      v = m_shunt_raw;
        else if (m_pointer == 0x02) v = m_bus_raw;
        else if (m_pointer == 0x03) v = m_power_raw;
        else if (m_pointer == 0x04) v = m_current_raw;
        else if (m_regs.count(m_pointer)) v = m_regs[m_pointer];
        if (len >= 1) out[0] = static_cast<uint8_t>(v >> 8);
        if (len >= 2) out[1] = static_cast<uint8_t>(v & 0xFF);
        for (uint8_t i = 2; i < len; ++i) out[i] = 0;
    }

private:
    void recompute() {
        const uint16_t cal = m_regs.count(0x05) ? m_regs[0x05] : 0;
        /* CURRENT = (SHUNT * CAL) / 2048  (datasheet eq.). */
        const int32_t cur =
            static_cast<int32_t>(static_cast<int16_t>(m_shunt_raw)) *
            static_cast<int32_t>(cal) / 2048;
        m_current_raw = static_cast<uint16_t>(static_cast<int16_t>(cur));
        /* POWER = (CURRENT * BUS) / 20000 (datasheet eq.). */
        const uint32_t p =
            (static_cast<uint32_t>(static_cast<uint16_t>(cur)) *
             m_bus_raw) / 20000;
        m_power_raw = static_cast<uint16_t>(p & 0xFFFFu);
    }

    std::map<uint8_t, uint16_t> m_regs;
    uint8_t  m_pointer     = 0;
    uint16_t m_shunt_raw   = 0;
    uint16_t m_bus_raw     = 0;
    uint16_t m_current_raw = 0;
    uint16_t m_power_raw   = 0;
};

/* ---------------- INA3221 ---------------- */

class Ina3221Model : public MockDevice {
public:
    Ina3221Model() {
        m_regs[0x00] = 0x7127;  /* config POR default */
        m_regs[0xFE] = 0x5449;
        m_regs[0xFF] = 0x3220;
    }

    /** Channel 0..2 (datasheet channels 1..3). */
    void setMeasurement(uint8_t ch, float bus_v, float shunt_v) {
        const long bus_raw   = lround(bus_v / 8e-3);
        const long shunt_raw = lround(shunt_v / 40e-6);
        const uint8_t reg_shunt = static_cast<uint8_t>(0x01 + 2 * ch);
        const uint8_t reg_bus   = static_cast<uint8_t>(0x02 + 2 * ch);
        m_regs[reg_shunt] =
            static_cast<uint16_t>(static_cast<int16_t>(shunt_raw) << 3);
        m_regs[reg_bus] =
            static_cast<uint16_t>(static_cast<uint16_t>(bus_raw) << 3);
    }

    bool onWrite(const uint8_t* data, uint8_t /*len*/) override {
        m_pointer = data[0];
        return true;  /* config writes accepted but not modeled */
    }

    void onRead(uint8_t* out, uint8_t len) override {
        const uint16_t v = m_regs.count(m_pointer) ? m_regs[m_pointer] : 0;
        if (len >= 1) out[0] = static_cast<uint8_t>(v >> 8);
        if (len >= 2) out[1] = static_cast<uint8_t>(v & 0xFF);
        for (uint8_t i = 2; i < len; ++i) out[i] = 0;
    }

private:
    std::map<uint8_t, uint16_t> m_regs;
    uint8_t m_pointer = 0;
};

/* ---------------- INA228 ---------------- */

class Ina228Model : public MockDevice {
public:
    explicit Ina228Model(float shunt_ohm) : m_shunt_ohm(shunt_ohm) {
        /* 16-bit registers are stored MSB-aligned in the 24-bit container. */
        m_regs[0x3E] = 0x5449u << 8;
        m_regs[0x3F] = 0x2280u << 8;  /* device id in bits 15:4, rev 0 */
        /* 24-bit result registers start at 0. */
        m_regs[0x04] = 0; m_regs[0x05] = 0; m_regs[0x06] = 0;
        m_regs[0x07] = 0; m_regs[0x08] = 0;
    }

    /** Physics: current = Vshunt / Rshunt; power = Vbus * I.  (The chip would
     * derive CURRENT from SHUNT_CAL; the driver decode of CURRENT x lsb must
     * return the physical current regardless.) */
    void setMeasurement(float bus_v, float shunt_v,
                        float current_lsb_a, float die_c = 45.0f) {
        const float current_a = (m_shunt_ohm > 0.0f) ? shunt_v / m_shunt_ohm : 0.0f;
        const long vsh_code = lround(shunt_v / 312.5e-9);      /* ADCRANGE=0 */
        const long bus_code = lround(bus_v / 195.3125e-6);
        const long cur_code = lround(current_a / current_lsb_a);
        const long pow_code = lround(static_cast<double>(bus_v) * current_a /
                                     (3.2 * current_lsb_a));
        const long tmp_code = lround(die_c / 7.8125e-3);
        set24(0x04, vsh_code);  /* VSHUNT */
        set24(0x05, bus_code);  /* VBUS */
        /* DIETEMP: 16-bit register (7.8125 mdegC/LSB), MSB-aligned. */
        m_regs[0x06] = (static_cast<uint32_t>(tmp_code) & 0xFFFFu) << 8;
        /* POWER saturates at 20 bits on the real part; clamp like hardware. */
        set24(0x07, cur_code);  /* CURRENT */
        set24(0x08, pow_code > 0xFFFFF ? 0xFFFFF : pow_code);  /* POWER */
    }

    uint16_t shuntCal() const { return static_cast<uint16_t>(m_regs.at(0x02) >> 8); }

    bool onWrite(const uint8_t* data, uint8_t len) override {
        m_pointer = data[0];
        if (len >= 3) {
            switch (m_pointer) {
                case 0x00: case 0x01: case 0x02: case 0x03:
                    m_regs[m_pointer] =
                        static_cast<uint32_t>((data[1] << 8) | data[2]) << 8;
                    break;  /* 16-bit registers stored MSB-aligned */
                default:
                    break;
            }
        }
        return true;
    }

    void onRead(uint8_t* out, uint8_t len) override {
        uint32_t v = m_regs.count(m_pointer) ? m_regs[m_pointer] : 0;
        /* 16-bit registers are stored MSB-aligned; reading 2 bytes returns
         * the top 16 bits exactly like the 16-bit parts. */
        if (len >= 1) out[0] = static_cast<uint8_t>(v >> 16);
        if (len >= 2) out[1] = static_cast<uint8_t>(v >> 8);
        if (len >= 3) out[2] = static_cast<uint8_t>(v);
        for (uint8_t i = 3; i < len; ++i) out[i] = 0;
    }

private:
    void set24(uint8_t reg, long code20) {
        /* 20-bit result, left-aligned (CODE<<4) in the 24-bit register. */
        const int32_t c = static_cast<int32_t>(code20) & 0x000FFFFF;
        const int32_t v = c << 4;
        m_regs[reg] = static_cast<uint32_t>(v) & 0x00FFFFFFu;
    }

    std::map<uint8_t, uint32_t> m_regs;
    uint8_t m_pointer = 0;
    float   m_shunt_ohm;
};

} // namespace hosttest
