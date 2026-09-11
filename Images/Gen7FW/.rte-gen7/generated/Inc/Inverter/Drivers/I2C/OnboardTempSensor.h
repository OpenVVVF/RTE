#pragma once

#include "Inverter/Drivers/I2C/I2cBus.h"

#include <cstdint>

namespace Inverter {

/**
 * @brief On-board digital temperature sensor (I2C5, PF0/PF1).
 *
 * The assembled part number is not yet known, so this driver targets the
 * ubiquitous pointer-register, 12-bit, 0.0625 degC/LSB family all of which
 * share register map and decode math:
 *   - TI TMP102   (no device-id register)
 *   - TI TMP1075  (DEVICE_ID reg 0x0F = 0x7500)
 *   - NXP PCT2075 (no device-id register)
 *
 * Register map (big-endian 16-bit words behind an 8-bit pointer):
 *   0x00 Temperature (12-bit code in bits 15:4; 13-bit when EM=1)
 *   0x01 Config      (EM = bit 4 selects 13-bit extended mode)
 *   0x02 Tlow, 0x03 Thigh -- unused here, kept for the alert pin feature
 *   0x0F DEVICE_ID   (TMP1075 only; reads 0x0000 elsewhere)
 *
 * init() probes with a short timeout and never blocks boot; a missing device
 * leaves part() == Part::None and the caller keeps re-probing lazily.
 *
 * All decode/encode math is static and free of bus state so the host test
 * harness (tests/host/) can verify vectors directly.
 */
class OnboardTempSensor {
public:
    enum class Part : uint8_t {
        None    = 0, /**< not detected / not initialised                  */
        Generic = 1, /**< TMP102 / PCT2075-compatible (no readable ID)    */
        Tmp1075 = 2, /**< DEVICE_ID 0x7500 matched                        */
    };

    static constexpr uint8_t  REG_TEMP      = 0x00;
    static constexpr uint8_t  REG_CONFIG    = 0x01;
    static constexpr uint8_t  REG_TLOW      = 0x02;
    static constexpr uint8_t  REG_THIGH     = 0x03;
    static constexpr uint8_t  REG_ID        = 0x0F;
    static constexpr uint16_t TMP1075_DEVICE_ID = 0x7500;
    static constexpr float    LSB_C         = 0.0625f;

    /**
     * @brief Probe the sensor and latch its identity.
     * @return true if the address ACKed (device present, even if the ID
     *         register reads back 0 -> Generic); false on NACK/bus error.
     */
    bool init(II2cBus& bus, uint8_t addr7, uint32_t timeout_ms);

    /**
     * @brief Read the temperature register and decode it.
     * @return false on bus error; @p temp_c untouched on failure.
     */
    bool poll(float& temp_c, uint32_t timeout_ms) const;

    Part    part() const          { return m_part; }
    uint8_t address() const       { return m_addr; }
    bool    extendedMode() const  { return m_extended; }
    const char* partName() const;

    /* -------- pure register math (host-testable) -------- */

    /**
     * @brief Decode the 16-bit temperature register to degC.
     * @param reg       Register value, host byte order (hi<<8|lo).
     * @param extended  true when config EM=1 (13-bit, value in bits 15:3).
     *
     * 12-bit (EM=0):  degC = arithmetic_sar(reg, 4) * 0.0625
     * 13-bit (EM=1):  degC = arithmetic_sar(reg, 3) * 0.0625
     * E.g. 0x7FF0 -> 127.9375 C; 0x1900 -> 25.0 C; EM 0x0C80 -> 25.0 C.
     */
    static float decodeTempC(uint16_t reg, bool extended);

    /** @brief Encode degC to a 16-bit temperature register (tests/mock). */
    static uint16_t encodeTempReg(float temp_c, bool extended);

    /** @brief true when config register value has the EM (13-bit) bit set. */
    static bool configIsExtended(uint16_t cfg) { return (cfg & 0x0010u) != 0u; }

private:
    bool readReg(uint8_t reg, uint16_t& value, uint32_t timeout_ms) const;
    bool writeReg(uint8_t reg, uint16_t value, uint32_t timeout_ms) const;

    II2cBus*  m_bus      = nullptr;
    uint8_t   m_addr     = 0;
    Part      m_part     = Part::None;
    bool      m_extended = false;
};

} // namespace Inverter
