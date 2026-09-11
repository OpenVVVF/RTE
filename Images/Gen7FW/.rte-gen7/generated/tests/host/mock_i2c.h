#pragma once

/**
 * @brief Mock I2C master + slave device register models for host-side driver
 *        verification (no MCU, no HAL — substitute for real Gen7 hardware).
 *
 * The models implement the register-visible behavior of the candidate parts:
 *   - Tmp1075Model:  TMP102/TMP1075/PCT2075 pointer-register family.
 *     TMP1075 flavor: DEVICE_ID (0x0F) = 0x7500, config POR 0x00FF (SBOS854F
 *     Table 7-5), no EM bit.  TMP102/PCT2075 have no ID register; the generic
 *     flavor masks the pointer to 2 bits like the TMP102 (assumed), so a
 *     probe at 0x0F aliases THIGH (0x5000).  Either way init() must classify
 *     the part as Generic and keep 12-bit decode.
 *   - Ina226Model:   16-bit part, MFG_ID(0xFE)=0x5449, DIE_ID(0xFF)=0x2260.
 *     Calibration transfer implemented per INA226 datasheet:
 *       CURRENT = (SHUNT * CAL) / 2048,  POWER = (CURRENT * BUS) / 20000.
 *   - Ina3221Model:  MFG_ID(0xFE)=0x5449, DIE_ID(0xFF)=0x3220, 3 channels,
 *     8 mV / 40 uV LSB in bits 15:3.  No CAL (current computed by firmware).
 *   - Ina228Model:   20-bit ADC in 24-bit registers, MFG_ID(0x3E)=0x5449,
 *     DEVICE_ID(0x3F)=0x2280 (id in bits 15:4 per Adafruit INA228 driver).
 *     CURRENT/POWER are computed from VSHUNT x the HELD SHUNT_CAL like the
 *     real silicon (identity: cur_code = vsh_code * 4096 / cal, verified for
 *     ADCRANGE=0 against the driver's cal formula), so a stale/wrong CAL or a
 *     stale CONFIG.ADCRANGE now perturbs readings exactly as on hardware.
 *     POWER is a straight 24-bit unsigned register (verified against Linux
 *     kernel ina238.c: 24-bit block read used unshifted; POWER_LIMIT is
 *     "compared against the 24-bit power register"); an earlier revision of
 *     this mock encoded it 20-bit left-aligned like VBUS/CURRENT — that was
 *     wrong and masked driver bug R1 (see test_rail_adversarial.cpp).
 *     ADCRANGE (CONFIG bit 4) is modelled: with ADCRANGE=1 the VSHUNT LSB is
 *     78.125 nV (4x finer), so a range-0 SHUNT_CAL reads currents 4x high.
 */

#include "Inverter/Drivers/I2C/I2cBus.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

namespace hosttest {

class MockDevice {
public:
    virtual ~MockDevice() = default;
    /** Handle write phase; data[0] is the register pointer. false = NACK. */
    virtual bool onWrite(const uint8_t* data, uint8_t len) = 0;
    /**
     * Whether the device ACKs the (repeated-start) read phase at the pointer
     * latched by the last write phase.  Default: ACK (all models).  Strict
     * variants override to NAK reads at unimplemented pointers.
     */
    virtual bool readPhaseAcks() const { return true; }
    /** Fill the read phase buffer at the current pointer. */
    virtual void onRead(uint8_t* out, uint8_t len) = 0;
};

class MockI2cBus : public Inverter::II2cBus {
public:
    /** One completed transfer as seen by the driver (for sequence checks).
     *  A lone-pointer write + N-byte read in a single transfer is the only
     *  way in II2cBus to do "pointer write, repeated START, read" (see
     *  I2cBus.h); tx_len/rx_len plus tx0 let tests assert exact shapes. */
    struct Txn {
        uint8_t addr;
        uint8_t tx0;     /**< register pointer byte (tx[0])            */
        uint8_t tx1;     /**< first data byte on writes (0 if absent)   */
        uint8_t tx2;     /**< second data byte on writes (0 if absent)  */
        uint8_t tx_len;
        uint8_t rx_len;
        bool    ok;
    };

    void attach(uint8_t addr7, MockDevice* dev) { m_devices[addr7] = dev; }
    void detach(uint8_t addr7) { m_devices.erase(addr7); }
    uint32_t transferCount() const { return m_transfers; }
    const std::vector<Txn>& txnLog() const { return m_log; }
    void clearLog() { m_log.clear(); }

    bool transfer(uint8_t addr7,
                  const uint8_t* tx, uint8_t tx_len,
                  uint8_t* rx, uint8_t rx_len,
                  uint32_t /*timeout_ms*/) override {
        ++m_transfers;
        Txn t{addr7,
              (tx != nullptr && tx_len > 0) ? tx[0] : static_cast<uint8_t>(0),
              (tx != nullptr && tx_len > 1) ? tx[1] : static_cast<uint8_t>(0),
              (tx != nullptr && tx_len > 2) ? tx[2] : static_cast<uint8_t>(0),
              tx_len, rx_len, false};
        auto it = m_devices.find(addr7);
        if (it == m_devices.end()) {
            m_log.push_back(t);
            return false;  /* NACK: nothing at this address */
        }
        if (tx != nullptr && tx_len > 0 && !it->second->onWrite(tx, tx_len)) {
            m_log.push_back(t);
            return false;
        }
        if (rx != nullptr && rx_len > 0) {
            if (!it->second->readPhaseAcks()) {
                m_log.push_back(t);
                return false;  /* strict device NAKs the read phase */
            }
            it->second->onRead(rx, rx_len);
        }
        t.ok = true;
        m_log.push_back(t);
        return true;
    }

    bool isReady(uint8_t addr7, uint32_t /*timeout_ms*/) override {
        return m_devices.count(addr7) != 0;
    }

private:
    std::map<uint8_t, MockDevice*> m_devices;
    std::vector<Txn> m_log;
    uint32_t m_transfers = 0;
};

/* ---------------- TMP102/TMP1075/PCT2075 ---------------- */

class Tmp1075Model : public MockDevice {
public:
    /**
     * @param has_id  true:  TMP1075 — DEVICE_ID 0x0F = 0x7500, 4-bit pointer
     *                mask, config POR 0x00FF (low "Not used" bits all read 1,
     *                SBOS854F Table 7-5).  The real TMP1075 has NO EM bit;
     *                the 0x00FF POR makes bit 4 read set, which is exactly
     *                why the driver must not trust EM on this part.
     *                false: TMP102-flavor generic — 2-bit pointer mask, so a
     *                probe at 0x0F aliases THIGH (0x5000) instead of reading
     *                0x0000 (SBOS397 pointer register: only P1/P0 used;
     *                masking of reserved bits assumed, not re-verified);
     *                config POR 0x60A0 (SBOS397, assumed — only the EM=0 bit
     *                matters to the driver).
     */
    explicit Tmp1075Model(bool has_id, uint8_t ptr_mask = 0)
        : m_ptr_mask(ptr_mask != 0 ? ptr_mask
                                   : (has_id ? 0x0F : 0x03)) {
        m_regs[0x00] = 0x1900;  /* 25.0 degC */
        m_regs[0x02] = 0x4B00;  /* Tlow 75 C */
        m_regs[0x03] = 0x5000;  /* Thigh 80 C */
        if (has_id) {
            m_regs[0x01] = 0x00FFu;  /* TMP1075 config POR */
            m_regs[0x0F] = 0x7500u;  /* DEVICE_ID */
        } else {
            m_regs[0x01] = 0x60A0u;  /* TMP102 config POR (EM=0) */
            /* no 0x0F register: the probe aliases THIGH via m_ptr_mask */
        }
    }

    void setTempRaw16(uint16_t reg) { m_regs[0x00] = reg; }
    void setConfig(uint16_t cfg)    { m_regs[0x01] = cfg; }
    uint16_t config() const         { return m_regs.at(0x01); }
    uint8_t pointer() const         { return m_pointer; }

    /** Make reads at (masked) pointer p NAK the read phase — e.g. a strict
     * part whose unimplemented pointers refuse the read instead of 0x0000
     * (assumed; datasheet silent on reserved-pointer behaviour). */
    void nakReadPointer(uint8_t p) { m_nak_reads.insert(p & m_ptr_mask); }

    bool onWrite(const uint8_t* data, uint8_t len) override {
        m_pointer = static_cast<uint8_t>(data[0] & m_ptr_mask);
        if (len >= 3) {
            m_regs[m_pointer] =
                static_cast<uint16_t>((data[1] << 8) | data[2]);
        }
        return true;
    }

    bool readPhaseAcks() const override {
        return m_nak_reads.count(m_pointer) == 0;
    }

    void onRead(uint8_t* out, uint8_t len) override {
        const uint16_t v = m_regs.count(m_pointer) ? m_regs[m_pointer] : 0;
        if (len >= 1) out[0] = static_cast<uint8_t>(v >> 8);
        if (len >= 2) out[1] = static_cast<uint8_t>(v & 0xFF);
        for (uint8_t i = 2; i < len; ++i) out[i] = 0;
    }

private:
    std::map<uint8_t, uint16_t> m_regs;
    std::set<uint8_t> m_nak_reads;
    uint8_t m_pointer  = 0;
    uint8_t m_ptr_mask;
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

    /** Drop-in raw register stimuli (e.g. 16-bit boundary patterns) — the
     * chip-computed current/power registers follow per the datasheet transfer
     * function, exactly as after setMeasurement(). */
    void setShuntRaw(uint16_t raw) { m_shunt_raw = raw; recompute(); }
    void setBusRaw(uint16_t raw)   { m_bus_raw = raw;   recompute(); }

    uint16_t cal() const        { return m_regs.at(0x05); }
    uint16_t currentReg() const { return m_current_raw; }
    uint16_t powerReg() const   { return m_power_raw; }

    /** Override the die ID (0xFF).  US-fab INA226 revs report 0x2261, so the
     * driver match must mask the RID nibble (SBOS547 Table 7-15 note). */
    void setDieId(uint16_t v) { m_regs[0xFF] = v; }

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

    /**
     * Channel 0..2 (datasheet channels 1..3).  Writes the channel's measured
     * registers ONLY when the channel is enabled in CONFIG — a disabled
     * channel stops converting and its registers keep their old contents.
     * (Channel-enable bits 14..12 verified vs Linux kernel ina3221.c,
     * CHx_EN(x) = BIT(14-x); POR 0x7127 enables all three.  The held-stale
     * contents themselves are 'assumed': the kernel driver refuses to read
     * disabled channels at all, so hardware behaviour there is unverified.)
     */
    void setMeasurement(uint8_t ch, float bus_v, float shunt_v) {
        if (!channelEnabled(ch)) return;
        const long bus_raw   = lround(bus_v / 8e-3);
        const long shunt_raw = lround(shunt_v / 40e-6);
        const uint8_t reg_shunt = static_cast<uint8_t>(0x01 + 2 * ch);
        const uint8_t reg_bus   = static_cast<uint8_t>(0x02 + 2 * ch);
        m_regs[reg_shunt] =
            static_cast<uint16_t>(static_cast<int16_t>(shunt_raw) << 3);
        m_regs[reg_bus] =
            static_cast<uint16_t>(static_cast<uint16_t>(bus_raw) << 3);
    }

    bool channelEnabled(uint8_t ch) const {
        const uint16_t cfg = configRaw();
        return (cfg & static_cast<uint16_t>(0x4000u >> ch)) != 0u;
    }
    void     setConfig(uint16_t cfg) { m_regs[0x00] = cfg; }
    uint16_t configRaw() const {
        return m_regs.count(0x00) ? m_regs.at(0x00) : 0x7127;
    }

    /** Override the die ID (0xFF) to emulate a different silicon revision
     * (0x3221 &c.) and prove the driver's RID-nibble-masked match. */
    void setDieId(uint16_t v) { m_regs[0xFF] = v; }

    bool onWrite(const uint8_t* data, uint8_t len) override {
        m_pointer = data[0];
        if (len >= 3 && m_pointer == 0x00) {
            m_regs[0x00] =
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

/* ---------------- INA228 ---------------- */

class Ina228Model : public MockDevice {
public:
    /** CONFIG bit 4 selects the high-resolution shunt range (verified via
     * Linux kernel ina238.c: INA238_CONFIG_ADCRANGE = BIT(4)). */
    static constexpr uint16_t CONFIG_ADCRANGE = 0x0010u;

    explicit Ina228Model(float shunt_ohm) : m_shunt_ohm(shunt_ohm) {
        /* 16-bit registers are stored MSB-aligned in the 24-bit container. */
        m_regs[0x00] = 0;                 /* CONFIG POR 0x0000: ADCRANGE=0 */
        m_regs[0x02] = 0x1000u << 8;      /* SHUNT_CAL POR 0x1000 (assumed: same
                                           * as the value the kernel programs) */
        m_regs[0x3E] = 0x5449u << 8;
        m_regs[0x3F] = 0x2281u << 8;  /* DEVID 0x228 in bits 15:4, rev 1 —
                                       * real parts read 0x2281, forcing the
                                       * driver to mask the revision nibble. */
        /* 24-bit result registers start at 0. */
        m_regs[0x04] = 0; m_regs[0x05] = 0; m_regs[0x06] = 0;
        m_regs[0x07] = 0; m_regs[0x08] = 0;
    }

    /**
     * @brief Set the electrical stimulus; result registers are derived the way
     *        the silicon derives them:
     *          VSHUNT  : 20-bit left-aligned, LSB 312.5 nV (ADCRANGE=0) or
     *                    78.125 nV (ADCRANGE=1)
     *          VBUS    : 20-bit left-aligned, LSB 195.3125 uV
     *          CURRENT : 20-bit left-aligned, = VSHUNT_code * 4096 / SHUNT_CAL
     *                    (chip computes from the held CAL, not from physics —
     *                    algebraically identical to (Vshunt/Rshunt)/Current_LSB
     *                    when CAL matches the driver's formula)
     *          POWER   : 24-bit straight value, LSB = 3.2 * Current_LSB,
     *                    = VBUS * CURRENT_code / 3.2, clamped at 0xFFFFFF
     *                    (saturation behaviour assumed).
     */
    void setMeasurement(float bus_v, float shunt_v, float die_c = 45.0f) {
        const bool range4 = (configRaw16() & CONFIG_ADCRANGE) != 0;
        const double vsh_lsb = range4 ? 78.125e-9 : 312.5e-9;
        const long vsh_code = lround(shunt_v / vsh_lsb);
        const long bus_code = lround(bus_v / 195.3125e-6);
        const double cal = m_regs.count(0x02)
                               ? static_cast<double>(m_regs[0x02] >> 8)
                               : 0.0;
        const long cur_code =
            (cal > 0.0) ? lround(static_cast<double>(vsh_code) * 4096.0 / cal)
                        : 0;
        long pow_code = lround(static_cast<double>(bus_v) *
                               static_cast<double>(cur_code) / 3.2);
        if (pow_code > 0x00FFFFFFL) pow_code = 0x00FFFFFFL;  /* assumed */
        const long tmp_code = lround(die_c / 7.8125e-3);
        set24(0x04, vsh_code);  /* VSHUNT: 20-bit left-aligned */
        set24(0x05, bus_code);  /* VBUS:   20-bit left-aligned */
        /* DIETEMP: 16-bit register (7.8125 mdegC/LSB), MSB-aligned. */
        m_regs[0x06] = (static_cast<uint32_t>(tmp_code) & 0xFFFFu) << 8;
        set24(0x07, cur_code);  /* CURRENT: 20-bit left-aligned */
        /* POWER: straight 24-bit value (verified vs Linux ina238.c:
         * read unshifted, LSB = 3.2 * Current_LSB). */
        m_regs[0x08] = static_cast<uint32_t>(pow_code) & 0x00FFFFFFu;
    }

    uint16_t shuntCal() const { return static_cast<uint16_t>(m_regs.at(0x02) >> 8); }
    uint32_t powerRaw24() const { return m_regs.at(0x08) & 0x00FFFFFFu; }

    /** Simulate warm-boot / externally-programmed register state. */
    void setConfigRaw16(uint16_t v) { m_regs[0x00] = static_cast<uint32_t>(v) << 8; }
    uint16_t configRaw16() const {
        return static_cast<uint16_t>(m_regs.at(0x00) >> 8);
    }
    void setDevId(uint16_t v) { m_regs[0x3F] = static_cast<uint32_t>(v) << 8; }

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
        /* 20-bit result, left-aligned (CODE<<4) in the 24-bit register
         * (VSHUNT/VBUS/CURRENT layout verified vs Linux ina238.c
         * ina238_read_field_s20: >>4 then sign-extend bit 19).  Overrange
         * wraps in the 20-bit field (assumed; saturation not verified). */
        const int32_t c = static_cast<int32_t>(code20) & 0x000FFFFF;
        const int32_t v = c << 4;
        m_regs[reg] = static_cast<uint32_t>(v) & 0x00FFFFFFu;
    }

    std::map<uint8_t, uint32_t> m_regs;
    uint8_t m_pointer = 0;
    float   m_shunt_ohm;
};

} // namespace hosttest
