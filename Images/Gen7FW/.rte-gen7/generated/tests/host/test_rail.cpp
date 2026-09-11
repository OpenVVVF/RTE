#include "test_framework.h"
#include "mock_i2c.h"

#include "Inverter/Drivers/I2C/RailMonitor.h"

#include <cstdio>

using Inverter::RailMonitor;
using hosttest::Ina226Model;
using hosttest::Ina228Model;
using hosttest::Ina3221Model;
using hosttest::MockI2cBus;

namespace {

constexpr float RSHUNT = 0.005f;  /* 5 mOhm */
constexpr float MAX_A  = 10.0f;   /* mission scenario: 5 mOhm / 10 A */

/* ---------- INA226 pure math ---------- */
void test_ina226_math() {
    std::printf("[rail] INA226 decode/cal math\n");
    const float lsb = RailMonitor::ina226CurrentLsb(MAX_A);
    CHECK_NEAR(lsb, 10.0 / 32768.0, 1e-9);

    /* CAL = 0.00512 / (Current_LSB * Rshunt) = 3355.44 truncated. */
    const uint16_t cal = RailMonitor::ina226CalValue(lsb, RSHUNT);
    CHECK(cal == 3355u);

    CHECK_NEAR(RailMonitor::ina226BusVFromRaw(19200), 24.0, 1e-3);   /* 1.25 mV/LSB */
    CHECK_NEAR(RailMonitor::ina226ShuntVFromRaw(10000), 0.025, 1e-7); /* 2.5 uV/LSB */
    CHECK_NEAR(RailMonitor::ina226ShuntVFromRaw(-5000), -0.0125, 1e-7);
    CHECK_NEAR(RailMonitor::ina226CurrentFromRaw(16384, lsb), 5.0, 1e-3);
    CHECK_NEAR(RailMonitor::ina226CurrentFromRaw(-8192, lsb), -2.5, 1e-3);
    CHECK_NEAR(RailMonitor::ina226PowerFromRaw(15725, lsb), 120.0, 0.06);
}

/* ---------- INA226 end-to-end via mock bus ---------- */
void test_ina226_detect_and_poll() {
    std::printf("[rail] INA226 detected at 0x40, 5 mOhm / 10 A\n");
    MockI2cBus bus;
    Ina226Model dev;
    dev.setMeasurement(24.0f, 0.025f);  /* 24 V bus, 25 mV shunt -> 5 A */
    bus.attach(0x40, &dev);

    RailMonitor mon;
    const RailMonitor::Config cfg{RSHUNT, MAX_A};
    CHECK(mon.init(bus, 0x40, cfg, 20));
    CHECK(mon.part() == RailMonitor::Part::INA226);
    CHECK(mon.channels() == 1);
    CHECK(mon.shuntCalValue() == 3355u);
    CHECK(dev.cal() == 3355u);
    CHECK(mon.lastMfgId() == 0x5449u);
    CHECK(mon.lastDevId() == 0x2260u);

    RailMonitor::Sample s{};
    CHECK(mon.poll(0, s, 10));
    /* current reg = 10000 * 3355 / 2048 = 16381 -> 16381 * 10/32768 A */
    CHECK_NEAR(s.bus_v, 24.0, 0.002);
    CHECK_NEAR(s.current_a, 4.9994, 0.005);
    CHECK_NEAR(s.power_w, 119.97, 0.10);

    /* Reverse current (regenerating): -12.5 mV shunt -> -2.5 A. */
    dev.setMeasurement(24.0f, -0.0125f);
    CHECK(mon.poll(0, s, 10));
    CHECK_NEAR(s.current_a, -2.4996, 0.005);
}

/* ---------- INA228 pure math ---------- */
void test_ina228_math() {
    std::printf("[rail] INA228 decode/cal math\n");
    const float lsb = RailMonitor::ina228CurrentLsb(MAX_A);
    CHECK_NEAR(lsb, 10.0 / 524288.0, 1e-12);

    /* SHUNT_CAL = 13107.2e6 * lsb * Rshunt = 1250 exactly at 10 A / 5 mOhm. */
    CHECK(RailMonitor::ina228ShuntCalValue(lsb, RSHUNT) == 1250u);

    /* 24 V bus: code = 24 / 195.3125 uV = 122880, reg = code << 4. */
    const uint32_t bus_reg = static_cast<uint32_t>(122880) << 4;
    CHECK_NEAR(RailMonitor::ina228BusVFromRaw(bus_reg), 24.0, 2e-4);

    const uint32_t cur_reg = static_cast<uint32_t>(104858) << 4;
    CHECK_NEAR(RailMonitor::ina228CurrentFromRaw(cur_reg, lsb), 2.0, 2e-5);

    /* Negative current: -1 A -> code -52429 rounds to -52429 -> two's
     * complement 20-bit left-aligned in 24-bit word. */
    const int32_t code = -52429;
    const uint32_t cur_reg_neg =
        (static_cast<uint32_t>(code) << 4) & 0x00FFFFFFu;
    CHECK_NEAR(RailMonitor::ina228CurrentFromRaw(cur_reg_neg, lsb), -1.0, 2e-4);

    CHECK_NEAR(RailMonitor::ina228PowerFromRaw(393216u << 4, lsb), 24.0, 5e-3);
    CHECK_NEAR(RailMonitor::ina228DieTempCFromRaw(5760), 45.0, 1e-3);

    /* sign extension */
    CHECK(RailMonitor::signExtend24(0x00400000) == 0x00400000);
    CHECK(RailMonitor::signExtend24(0x00F00000) < 0);
    CHECK(RailMonitor::signExtend24(0x00000000) == 0);
}

/* ---------- INA228 end-to-end via mock bus ---------- */
void test_ina228_detect_and_poll() {
    std::printf("[rail] INA228 detected at 0x41, 5 mOhm / 10 A\n");
    MockI2cBus bus;
    Ina228Model dev(RSHUNT);
    bus.attach(0x41, &dev);

    RailMonitor mon;
    const RailMonitor::Config cfg{RSHUNT, MAX_A};
    CHECK(mon.init(bus, 0x41, cfg, 20));
    CHECK(mon.part() == RailMonitor::Part::INA228);
    CHECK(mon.channels() == 1);
    CHECK(mon.shuntCalValue() == 1250u);
    CHECK(dev.shuntCal() == 1250u);

    const float lsb = mon.currentLsbA();
    dev.setMeasurement(12.0f, 0.010f, lsb);  /* 12 V, 10 mV -> 2 A, 24 W */

    RailMonitor::Sample s{};
    CHECK(mon.poll(0, s, 10));
    CHECK_NEAR(s.bus_v, 12.0, 2e-4);
    CHECK_NEAR(s.current_a, 2.0, 1e-4);
    CHECK_NEAR(s.power_w, 24.0, 2e-3);
}

/* ---------- INA3221 pure math + end-to-end ---------- */
void test_ina3221_math_and_poll() {
    std::printf("[rail] INA3221 3-channel at 0x40, 5 mOhm\n");
    CHECK_NEAR(RailMonitor::ina3221BusVFromRaw(0x5DC0), 24.0, 1e-3);
    CHECK_NEAR(RailMonitor::ina3221ShuntVFromRaw(0x1388), 0.025, 1e-7);
    /* Negative shunt: -312 raw -> reg 0xF640 -> -12.48 mV. */
    CHECK_NEAR(RailMonitor::ina3221ShuntVFromRaw(
                   static_cast<int16_t>(0xF640)), -0.01248, 1e-7);
    CHECK_NEAR(RailMonitor::ina3221CurrentA(0.025f, RSHUNT), 5.0, 1e-6);

    MockI2cBus bus;
    Ina3221Model dev;
    bus.attach(0x40, &dev);
    dev.setMeasurement(0, 24.0f, 0.025f);   /* ch1: 24 V, 5 A  */
    dev.setMeasurement(1, 3.3f,  0.0025f);  /* ch2: 3.3 V, 0.5 A */
    dev.setMeasurement(2, 5.0f,  0.0f);     /* ch3: 5 V,  0 A   */

    RailMonitor mon;
    const RailMonitor::Config cfg{RSHUNT, MAX_A};
    CHECK(mon.init(bus, 0x40, cfg, 20));
    CHECK(mon.part() == RailMonitor::Part::INA3221);
    CHECK(mon.channels() == 3);
    CHECK(mon.shuntCalValue() == 0u);  /* no CAL register on this part */

    RailMonitor::Sample s{};
    CHECK(mon.poll(0, s, 10));
    CHECK_NEAR(s.bus_v, 24.0, 0.005);
    CHECK_NEAR(s.current_a, 5.0, 0.002);
    CHECK_NEAR(s.power_w, 120.0, 0.05);

    CHECK(mon.poll(1, s, 10));
    CHECK_NEAR(s.bus_v, 3.3, 0.005);
    CHECK_NEAR(s.current_a, 0.5, 0.01);
    CHECK_NEAR(s.power_w, 1.65, 0.03);

    CHECK(mon.poll(2, s, 10));
    CHECK_NEAR(s.bus_v, 5.0, 0.005);
    CHECK_NEAR(s.current_a, 0.0, 0.01);

    /* Out-of-range channel must fail, not wrap. */
    CHECK(!mon.poll(3, s, 10));
}

/* ---------- auto-detect order / negative paths ---------- */
void test_absent() {
    std::printf("[rail] absent device (NACK on probe address)\n");
    MockI2cBus bus;  /* empty bus */
    RailMonitor mon;
    const RailMonitor::Config cfg{RSHUNT, MAX_A};
    CHECK(!mon.init(bus, 0x40, cfg, 20));
    CHECK(mon.part() == RailMonitor::Part::None);
    CHECK(mon.channels() == 0);
    RailMonitor::Sample s{1.0f, 1.0f, 1.0f};
    CHECK(!mon.poll(0, s, 10));
    CHECK(bus.transferCount() == 0);  /* isReady answered the probe */
}

void test_unknown_die() {
    std::printf("[rail] unknown die id -> not detected\n");
    MockI2cBus bus;
    Ina226Model dev;
    bus.attach(0x40, &dev);
    /* Corrupt the die id: 0xFE/0xFF are read-only in the model, so emulate a
     * different die by re-attaching a bare device with foreign IDs. */
    class ForeignTi : public hosttest::MockDevice {
     public:
        bool onWrite(const uint8_t* d, uint8_t) override { m_ptr = d[0]; return true; }
        void onRead(uint8_t* out, uint8_t len) override {
            uint16_t v = 0;
            if (m_ptr == 0xFE) v = 0x5449;
            if (m_ptr == 0xFF) v = 0x9999;  /* some future TI part */
            if (len >= 1) out[0] = static_cast<uint8_t>(v >> 8);
            if (len >= 2) out[1] = static_cast<uint8_t>(v & 0xFF);
            for (uint8_t i = 2; i < len; ++i) out[i] = 0;
        }
     private:
        uint8_t m_ptr = 0;
    } foreign;
    MockI2cBus bus2;
    bus2.attach(0x44, &foreign);

    RailMonitor mon;
    const RailMonitor::Config cfg{RSHUNT, MAX_A};
    CHECK(!mon.init(bus2, 0x44, cfg, 20));
    CHECK(mon.part() == RailMonitor::Part::None);
    CHECK(mon.lastMfgId() == 0x5449u);
    CHECK(mon.lastDevId() == 0x9999u);
}

void test_bad_config_rejected() {
    std::printf("[rail] invalid calibration config rejected\n");
    MockI2cBus bus;
    Ina226Model dev;
    bus.attach(0x40, &dev);
    RailMonitor mon;
    CHECK(!mon.init(bus, 0x40, RailMonitor::Config{0.0f, MAX_A}, 20));
    CHECK(!mon.init(bus, 0x40, RailMonitor::Config{RSHUNT, 0.0f}, 20));
    CHECK(!mon.init(bus, 0x40, RailMonitor::Config{-0.005f, MAX_A}, 20));
    CHECK(mon.part() == RailMonitor::Part::None);
    CHECK(bus.transferCount() == 0);  /* rejected before touching the bus */
}

} // namespace

void hosttest::test_rail_suite() {
    test_ina226_math();
    test_ina226_detect_and_poll();
    test_ina228_math();
    test_ina228_detect_and_poll();
    test_ina3221_math_and_poll();
    test_absent();
    test_unknown_die();
    test_bad_config_rejected();
}
