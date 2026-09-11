#pragma once

#include "Inverter/Drivers/I2C/I2cBus.h"

#include <cstdint>
#include <cmath>

namespace Inverter {

/**
 * @brief TI rail monitor on I2C4 (PD12/PF15): bus voltage + shunt current
 *        (+ derived power) for one monitored rail.
 *
 * The assembled part number is not yet known, so init() auto-detects the
 * actual device with a capability/ID table, trying each candidate family:
 *
 *   | Part    | ID check                        | Shunt present | CAL reg |
 *   |---------|---------------------------------|---------------|---------|
 *   | INA226  | reg 0xFE=0x5449, 0xFF=0x2260    | 0x01 (16b)    | 0x05    |
 *   | INA3221 | reg 0xFE=0x5449, 0xFF=0x3220    | 0x01/03/05    | none    |
 *   | INA228  | reg 0x3E=0x5449, (0x3F>>4)=0x228| 0x04 (24b)    | 0x02    |
 *
 * INA226/INA3221 probe first (they alias register space with INA228's ID
 * registers); the INA228 probe only runs when the 0xFE/0xFF reads fail or
 * mismatch, so a 16-bit part can never be mistaken for a 20-bit one.
 *
 * All calculations run from a single calibration model per part; the raw
 * decode/encode math is static and host-testable (tests/host/).
 */
class RailMonitor {
public:
    enum class Part : uint8_t {
        None    = 0,
        INA226  = 1,
        INA228  = 2,
        INA3221 = 3,
    };

    struct Config {
        float shunt_ohm;       /**< shunt resistor [ohm]                    */
        float max_expected_a;  /**< max expected rail current [A] (CAL ref) */
    };

    struct Sample {
        float bus_v;
        float current_a;
        float power_w;
    };

    static constexpr uint8_t MAX_CHANNELS = 3; /**< INA3221 channel count */

    /* Shared register addresses. */
    static constexpr uint8_t REG_INA226_CONFIG    = 0x00;
    static constexpr uint8_t REG_INA226_SHUNT     = 0x01;
    static constexpr uint8_t REG_INA226_BUS       = 0x02;
    static constexpr uint8_t REG_INA226_POWER     = 0x03;
    static constexpr uint8_t REG_INA226_CURRENT   = 0x04;
    static constexpr uint8_t REG_INA226_CAL       = 0x05;
    static constexpr uint8_t REG_INA226_MASK_EN   = 0x06;
    /* Mask/Enable (0x06) bit 2 = OVF math overflow: shunt/current/power
     * math overflowed (|current| beyond the calibrated full-scale). */
    static constexpr uint16_t INA226_OVF          = 0x0004;
    static constexpr uint8_t REG_INA228_CONFIG    = 0x00;
    /* INA228 CONFIG bit 4 = ADCRANGE (0: +/-163.84mV, 1: +/-40.96mV). */
    static constexpr uint16_t INA228_CFG_ADCRANGE = 0x0010;
    /* INA226/INA3221 CONFIG mode bits [2:0] = 111: continuous shunt+bus. */
    static constexpr uint16_t INA226_CFG_MODE_CONT = 0x0007;
    /* INA3221 CONFIG bits 14:12 = CH1..CH3 enable. */
    static constexpr uint16_t INA3221_CFG_CH_EN    = 0x7000;
    static constexpr uint8_t REG_INA_MFG_ID       = 0xFE; /**< 226 + 3221 */
    static constexpr uint8_t REG_INA_DIE_ID       = 0xFF; /**< 226 + 3221 */

    static constexpr uint8_t REG_INA228_SHUNT_CAL = 0x02;
    static constexpr uint8_t REG_INA228_VSHUNT    = 0x04;
    static constexpr uint8_t REG_INA228_VBUS      = 0x05;
    static constexpr uint8_t REG_INA228_DIETEMP   = 0x06;
    static constexpr uint8_t REG_INA228_CURRENT   = 0x07;
    static constexpr uint8_t REG_INA228_POWER     = 0x08;
    static constexpr uint8_t REG_INA228_MFG_ID    = 0x3E;
    static constexpr uint8_t REG_INA228_DEV_ID    = 0x3F;

    /* ID-register expectations (TI manufacturer ID is ASCII "TI").      */
    static constexpr uint16_t TI_MFG_ID        = 0x5449;
    static constexpr uint16_t INA226_DIE_ID    = 0x2260;
    static constexpr uint16_t INA3221_DIE_ID   = 0x3220;
    static constexpr uint16_t INA228_DEV_ID_HI = 0x2280; /**< id<<4, rev=0 */

    /**
     * @brief Probe + identify + configure the rail monitor at @p addr7.
     * @return true when a known part was found and configured.
     *         false on NACK (nothing there) or an unrecognised ID pair
     *         (the raw IDs remain readable via lastMfgId()/lastDevId()).
     */
    bool init(II2cBus& bus, uint8_t addr7, const Config& cfg, uint32_t timeout_ms);

    /**
     * @brief Read one rail's bus voltage / shunt current / derived power.
     * @param channel  INA3221: 0..2; single-channel parts ignore it (use 0).
     * @return false on bus error or uninitialised device; out untouched.
     */
    bool poll(uint8_t channel, Sample& out, uint32_t timeout_ms) const;

    Part        part() const          { return m_part; }
    uint8_t     channels() const      { return (m_part == Part::INA3221) ? MAX_CHANNELS
                                               : (m_part == Part::None ? 0 : 1); }
    const char* partName() const;
    uint8_t     address() const       { return m_addr; }
    uint16_t    shuntCalValue() const { return m_cal; }
    uint16_t    lastMfgId() const     { return m_last_mfg; }
    uint16_t    lastDevId() const     { return m_last_dev; }
    float       currentLsbA() const   { return m_current_lsb; }

    /* -------- INA226 pure math (host-testable) --------
     * Current_LSB = MaxExpectedA / 32768;  CAL = 0.00512 / (Current_LSB*Rsh)
     * Bus LSB = 1.25 mV; shunt LSB = 2.5 uV; power LSB = 25 * Current_LSB. */
    static float    ina226CurrentLsb(float max_expected_a);
    static uint16_t ina226CalValue(float current_lsb_a, float shunt_ohm);
    static float    ina226BusVFromRaw(uint16_t raw)      { return static_cast<float>(raw) * 1.25e-3f; }
    static float    ina226ShuntVFromRaw(int16_t raw)     { return static_cast<float>(raw) * 2.5e-6f; }
    static float    ina226CurrentFromRaw(int16_t raw, float current_lsb_a);
    static float    ina226PowerFromRaw(uint16_t raw, float current_lsb_a);

    /* -------- INA228 pure math (20-bit ADC, 24-bit registers) --------
     * Current_LSB = MaxExpectedA/2^19; SHUNT_CAL = 13107.2e6 * LSB * Rsh
     * (ADCRANGE=0).  Results sit left-aligned in 24-bit registers; Vbus LSB
     * = 195.3125 uV; power LSB = 3.2 * Current_LSB. */
    static float    ina228CurrentLsb(float max_expected_a);
    static uint16_t ina228ShuntCalValue(float current_lsb_a, float shunt_ohm);
    static int32_t  signExtend24(uint32_t raw24);
    static float    ina228BusVFromRaw(uint32_t raw24);
    static float    ina228CurrentFromRaw(uint32_t raw24, float current_lsb_a);
    static float    ina228PowerFromRaw(uint32_t raw24, float current_lsb_a);
    static float    ina228DieTempCFromRaw(uint16_t raw);

    /* -------- INA3221 pure math (no CAL: current computed in firmware) --
     * Bus LSB = 8 mV (bits 15:3); shunt LSB = 40 uV (signed, bits 15:3). */
    static float    ina3221BusVFromRaw(uint16_t raw)   { return static_cast<float>(raw >> 3) * 8e-3f; }
    static float    ina3221ShuntVFromRaw(int16_t raw)  { return static_cast<float>(raw >> 3) * 40e-6f; }
    static float    ina3221CurrentA(float shunt_v, float shunt_ohm);

private:
    bool readReg16(uint8_t reg, uint16_t& value, uint32_t timeout_ms) const;
    bool writeReg16(uint8_t reg, uint16_t value, uint32_t timeout_ms) const;
    bool readReg24(uint8_t reg, uint32_t& value, uint32_t timeout_ms) const;

    II2cBus*  m_bus         = nullptr;
    uint8_t   m_addr        = 0;
    Part      m_part        = Part::None;
    uint16_t  m_cal         = 0;
    float     m_current_lsb = 0.0f;
    float     m_shunt_ohm   = 0.0f;
    uint16_t  m_last_mfg    = 0;
    uint16_t  m_last_dev    = 0;
};

} // namespace Inverter
