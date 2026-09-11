/* Adversarial coverage for RailMonitor (INA226 / INA228 / INA3221).
 *
 * Provenance tags (see also test_temp_adversarial.cpp):
 *   [V] verified against public datasheet-derived sources this session:
 *       Linux kernel ina2xx.c / ina238.c / ina3221.c (TI-authored or
 *       datasheet-derived, hardware-tested), Adafruit INA228 library.
 *   [A] assumed / mock-informed: mock and driver share the assumption and it
 *       could not be re-verified against datasheet text.
 *   [D] documented/characterized driver behavior.
 */
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
constexpr float MAX_A  = 10.0f;

/* ---------- INA226: calibration that does NOT divide evenly ---------- */

void test_ina226_cal_worked_example() {
    std::printf("[rail-adv] INA226 CAL: uneven + datasheet worked example\n");
    /* Task-cited worked example: with Current_LSB = 1 mA/bit and Rshunt =
     * 2 mOhm the CAL register is exactly 0x0A00 = 2560 (CAL = 0.00512 /
     * (LSB*R); the 0.00512 constant is corroborated by the kernel ina2xx
     * fixed-CAL=2048 scheme).  The "3 A / 10 mOhm" parameterization of that
     * example could not be re-verified against the datasheet PDF (not
     * fetchable) — provenance [A]; the formula case below is exact. */
    CHECK(RailMonitor::ina226CalValue(0.001f, 0.002f) == 0x0A00u);

    /* Same example through the driver's own convention (LSB = max/2^15):
     * LSB = 91.552734375 uA -> CAL = 5592.4 -> rounds to 5592 (0x15D8), NOT
     * 0x0A00 — that example number presumes a normalized LSB.  [D] */
    const float lsb3a = RailMonitor::ina226CurrentLsb(3.0f);
    CHECK_NEAR(lsb3a, 3.0 / 32768.0, 1e-12);
    CHECK(RailMonitor::ina226CalValue(lsb3a, 0.01f) == 5592u);
}

void test_ina226_end_to_end_uneven_cal() {
    std::printf("[rail-adv] INA226 e2e, 3 A / 10 mOhm (uneven CAL)\n");
    MockI2cBus bus;
    Ina226Model dev;
    bus.attach(0x42, &dev);

    RailMonitor mon;
    CHECK(mon.init(bus, 0x42, RailMonitor::Config{0.01f, 3.0f}, 20));
    CHECK(mon.shuntCalValue() == 5592u);
    CHECK(dev.cal() == 5592u);

    /* Full-scale-ish: 30 mV shunt = 3.0 A on a 10 mOhm shunt. */
    dev.setMeasurement(12.0f, 0.030f);
    RailMonitor::Sample s{};
    CHECK(mon.poll(0, s, 10));
    CHECK_NEAR(s.bus_v, 12.0, 0.002);
    /* reg = 12000*5592/2048 = 32765 (chip truncates) -> 32765*3/32768 A */
    CHECK_NEAR(s.current_a, 2.9997, 5e-4);
    /* reg = 32765*9600/20000 = 15727 -> 15727*25*3/32768 = 35.9959 W */
    CHECK_NEAR(s.power_w, 36.0, 0.01);
}

/* Slightly over max_expected_a, the INA226 current register wraps past
 * int16 full scale.  Physical rail is +10.5 A; the driver reports a large
 * NEGATIVE current.  [A: two's-complement wrap is mock-informed; the real
 * part at least sets MATH_OVERFLOW (OVF) in Mask/Enable — which this driver
 * never reads.  See report.] */
void test_ina226_overrange_wraps_negative() {
    std::printf("[rail-adv] INA226 over-max current wraps (10 mOhm? no, 5 mOhm, 10 A max)\n");
    MockI2cBus bus;
    Ina226Model dev;
    bus.attach(0x40, &dev);

    RailMonitor mon;
    CHECK(mon.init(bus, 0x40, RailMonitor::Config{RSHUNT, MAX_A}, 20));

    dev.setMeasurement(24.0f, 0.0525f);  /* 52.5 mV -> 10.5 A > 10 A max */
    RailMonitor::Sample s{};
    CHECK(mon.poll(0, s, 10));
    /* reg = 21000*3355/2048 = 34401 -> wraps int16 to -31135 */
    CHECK(dev.currentReg() == static_cast<uint16_t>(-31135));
    CHECK_NEAR(s.current_a, -9.5016, 0.001);
    /* nonsense-positive power follows the wrapped register */
    CHECK_NEAR(s.power_w, 251.95, 0.05);
}

/* Raw shunt 0x8000 = -32768 codes (one LSB beyond +full-scale magnitude):
 * the register math overflows twice.  Physical truth: -81.92 mV = -16.38 A
 * on a 5 mOhm shunt; the driver reports +3.62 A.  [A: wrap behavior is
 * mock-informed].  Exercises the driver's int16 path on the min pattern. */
void test_ina226_shunt_raw_min_negative() {
    std::printf("[rail-adv] INA226 shunt raw 0x8000 (min negative)\n");
    MockI2cBus bus;
    Ina226Model dev;
    bus.attach(0x40, &dev);

    RailMonitor mon;
    CHECK(mon.init(bus, 0x40, RailMonitor::Config{RSHUNT, MAX_A}, 20));

    dev.setShuntRaw(0x8000);
    dev.setBusRaw(28800);  /* 36 V */
    RailMonitor::Sample s{};
    CHECK(mon.poll(0, s, 10));
    /* -32768*3355/2048 = -53680 -> int16 wrap 11856 */
    CHECK(dev.currentReg() == 11856u);
    CHECK_NEAR(s.current_a, 3.6182, 0.001);
    CHECK_NEAR(s.power_w, 130.25, 0.01);
}

void test_ina226_cal_clamps() {
    std::printf("[rail-adv] INA226 CAL register clamps\n");
    /* Tiny LSB * tiny shunt -> CAL far past 0x7FFF: driver clamps to 0x7FFF.
     * [A: the "bit 15 reserved" rationale is from the driver comment; the
     * clamp itself is driver design — flagged for the datasheet review.] */
    CHECK(RailMonitor::ina226CalValue(RailMonitor::ina226CurrentLsb(0.1f),
                                      0.0005f) == 0x7FFFu);
    /* Huge LSB * shunt -> CAL rounds to 0: driver clamps up to 1 so the
     * chip's current/power registers stay defined. */
    CHECK(RailMonitor::ina226CalValue(RailMonitor::ina226CurrentLsb(13107.2f),
                                      0.1f) == 1u);
    /* Full-scale power decode headroom: 65535 * 25 * LSB.  (Unreachable in
     * rated use: 36 V bus * full-scale current ~ 47184 codes.) */
    CHECK_NEAR(RailMonitor::ina226PowerFromRaw(
                   0xFFFFu, RailMonitor::ina226CurrentLsb(MAX_A)),
               499.99, 0.02);
}

/* ---------- INA228: 20-bit boundaries, ADCRANGE, big power ---------- */

void test_ina228_20bit_boundaries() {
    std::printf("[rail-adv] INA228 20-bit sign/boundary decode\n");
    const float lsb = RailMonitor::ina228CurrentLsb(MAX_A);  /* 10/2^19 */
    /* Min negative code (-524288): raw 0x800000 -> exactly -max_expected_a */
    CHECK_NEAR(RailMonitor::ina228CurrentFromRaw(0x00800000u, lsb), -10.0, 1e-4);
    /* Max positive code (524287): raw 0x7FFFF0 */
    CHECK_NEAR(RailMonitor::ina228CurrentFromRaw(0x007FFFF0u, lsb), 9.99998, 1e-4);
    /* -1 code: raw 0xFFFFF0 — top nibble 0xF is sign extension of bit 19 */
    CHECK_NEAR(RailMonitor::ina228CurrentFromRaw(0x00FFFFF0u, lsb),
               -1.90735e-5, 1e-7);
    /* Reserved low nibble (bits 3:0 always 0 on the part [V: kernel comment])
     * must not leak into the code if ever set by noise: >>4 discards. */
    CHECK_NEAR(RailMonitor::ina228CurrentFromRaw(0x00000008u, lsb), 0.0, 1e-9);
    /* VBUS at the 85 V common-mode ceiling: 435200 << 4 = 0x6A4000. */
    CHECK_NEAR(RailMonitor::ina228BusVFromRaw(0x006A4000u), 85.0, 1e-3);
    /* [D] VBUS shares the signed-20-bit decode: a bit-19-set pattern decodes
     * negative (-102.4 V).  Physically unreachable (max 85 V = code 435200),
     * but the symmetric decode is characterized here, same as the kernel's
     * read_field_s20 path.  [V] */
    CHECK_NEAR(RailMonitor::ina228BusVFromRaw(0x00800000u), -102.4, 0.01);
    /* Negative die temperature: 0xE800 = -6144 * 7.8125 mC = -48 C. */
    CHECK_NEAR(RailMonitor::ina228DieTempCFromRaw(0xE800u), -48.0, 1e-3);
    CHECK(RailMonitor::signExtend24(0x00FFFFFFu) == -1);
    CHECK(RailMonitor::signExtend24(0x007FFFFFu) == 0x007FFFFF);
}

/* Power needing >20 bits: 80 V * 9.5 A = 760 W -> power code 12,451,850,
 * which fills bits 23:21.  Regression guard for the fixed 24-bit decode —
 * a 20-bit-left-aligned decode would lose this entirely.  [V: Linux ina238.c
 * reads POWER as straight 24-bit.] */
void test_ina228_large_power_24bit() {
    std::printf("[rail-adv] INA228 power register >20 bits\n");
    MockI2cBus bus;
    Ina228Model dev(RSHUNT);
    bus.attach(0x41, &dev);

    RailMonitor mon;
    CHECK(mon.init(bus, 0x41, RailMonitor::Config{RSHUNT, MAX_A}, 20));
    CHECK(mon.shuntCalValue() == 1250u);
    CHECK(dev.shuntCal() == 1250u);

    dev.setMeasurement(80.0f, 0.0475f);  /* 47.5 mV / 5 mOhm = 9.5 A; 760 W */
    RailMonitor::Sample s{};
    CHECK(mon.poll(0, s, 10));
    CHECK_NEAR(s.bus_v, 80.0, 0.01);
    CHECK_NEAR(s.current_a, 9.5001, 1e-3);
    CHECK_NEAR(s.power_w, 760.0, 0.1);
    CHECK(dev.powerRaw24() == 12451850u);  /* 0xBDF4CA, bits above 19 set */
}

/* Warm-boot hazard: the part was left in ADCRANGE=1 (4x finer shunt LSB) by
 * a previous run.  init() now RMWs CONFIG to force ADCRANGE=0, so the
 * range-0 SHUNT_CAL the driver computes stays consistent.  [V: ADCRANGE bit
 * position and the x4 SHUNT_CAL rule — kernel ina238.c and Adafruit INA228;
 * A: the mock's chip-internal coupling model.] */
void test_ina228_stale_adcrange() {
    std::printf("[rail-adv] INA228 stale ADCRANGE=1 cleared at init\n");
    MockI2cBus bus;
    Ina228Model dev(RSHUNT);
    dev.setConfigRaw16(Ina228Model::CONFIG_ADCRANGE);  /* warm-boot state */
    bus.attach(0x41, &dev);

    RailMonitor mon;
    CHECK(mon.init(bus, 0x41, RailMonitor::Config{RSHUNT, MAX_A}, 20));
    CHECK(mon.part() == RailMonitor::Part::INA228);
    /* init must have CLEARED ADCRANGE with a CONFIG read-modify-write. */
    CHECK((dev.configRaw16() & Ina228Model::CONFIG_ADCRANGE) == 0u);

    dev.setMeasurement(12.0f, 0.010f);  /* physical: 2 A, 24 W */
    RailMonitor::Sample s{};
    CHECK(mon.poll(0, s, 10));
    CHECK_NEAR(s.current_a, 2.0, 1e-3);   /* correct after the RMW */
    CHECK_NEAR(s.power_w, 24.0, 0.02);
}

/* Revision-nibble masking on every family's ID registers.  [V: the INA228
 * mock's default 0x2281 (rev 1) already exercises one mask path; INA226 US
 * fab revs report 0x2261 per SBOS547 note per parent.] */
void test_die_id_revision_masking() {
    std::printf("[rail-adv] die-ID revision nibble masking\n");
    {
        MockI2cBus bus;
        Ina226Model dev;
        dev.setDieId(0x2261);  /* silicon rev 1 */
        bus.attach(0x40, &dev);
        RailMonitor mon;
        CHECK(mon.init(bus, 0x40, RailMonitor::Config{RSHUNT, MAX_A}, 20));
        CHECK(mon.part() == RailMonitor::Part::INA226);
        CHECK(mon.lastDevId() == 0x2261u);
    }
    {
        MockI2cBus bus;
        Ina3221Model dev;
        dev.setDieId(0x3227);
        bus.attach(0x40, &dev);
        RailMonitor mon;
        CHECK(mon.init(bus, 0x40, RailMonitor::Config{RSHUNT, MAX_A}, 20));
        CHECK(mon.part() == RailMonitor::Part::INA3221);
        CHECK(mon.lastDevId() == 0x3227u);
    }
    {   /* 0x2290: a different die in the same package style must NOT match. */
        MockI2cBus bus;
        Ina228Model dev(RSHUNT);
        dev.setDevId(0x2290);
        bus.attach(0x41, &dev);
        RailMonitor mon;
        CHECK(!mon.init(bus, 0x41, RailMonitor::Config{RSHUNT, MAX_A}, 20));
        CHECK(mon.part() == RailMonitor::Part::None);
        /* The 0x3E/0x3F probe records the raw IDs on rejection too, keeping
         * the header's "raw IDs remain readable" contract for unknown parts. */
        CHECK(mon.lastMfgId() == 0x5449u);
        CHECK(mon.lastDevId() == 0x2290u);
    }
}

/* ---------- INA3221: disabled channels ---------- */

/* Driver has no channel-enable awareness: it neither reads nor writes the
 * INA3221 CONFIG register, so channels() is always 3 and poll() reads a
 * disabled channel's registers without complaint.  [V: kernel ina3221.c
 * refuses to report disabled channels (-ENODATA); the RTE driver returns
 * their stale/zero contents as if live — reported.] */
void test_ina3221_disabled_channel_reads() {
    std::printf("[rail-adv] INA3221 disabled channel read-through\n");
    MockI2cBus bus;
    Ina3221Model dev;
    dev.setConfig(0x5127);     /* CH2 (bit 13) disabled before any "ADC run" */
    bus.attach(0x40, &dev);
    dev.setMeasurement(0, 24.0f, 0.025f);
    dev.setMeasurement(1, 3.3f, 0.0025f);   /* dropped: channel disabled */
    dev.setMeasurement(2, 5.0f, 0.0f);

    RailMonitor mon;
    CHECK(mon.init(bus, 0x40, RailMonitor::Config{RSHUNT, MAX_A}, 20));
    CHECK(mon.part() == RailMonitor::Part::INA3221);
    CHECK(mon.channels() == 3);  /* advertised regardless of CH_EN bits [D] */

    RailMonitor::Sample s{9.0f, 9.0f, 9.0f};
    CHECK(mon.poll(0, s, 10));
    CHECK_NEAR(s.bus_v, 24.0, 0.005);
    /* Disabled channel: poll() SUCCEEDS but returns never-converted zeros —
     * indistinguishable from a genuinely idle rail. [A: held-value vs zero
     * on disabled channels unverified vs datasheet text] */
    CHECK(mon.poll(1, s, 10));
    CHECK_NEAR(s.bus_v, 0.0, 1e-6);
    CHECK_NEAR(s.current_a, 0.0, 1e-6);
}

/* Disabled AFTER a conversion: registers hold the last value, so the driver
 * reports stale data as live. [A: hold behavior mock-informed] */
void test_ina3221_disabled_channel_stale() {
    std::printf("[rail-adv] INA3221 disable-after-convert returns stale\n");
    MockI2cBus bus;
    Ina3221Model dev;
    bus.attach(0x40, &dev);
    dev.setMeasurement(1, 3.3f, 0.0025f);  /* converted while enabled */
    dev.setConfig(0x5127);                 /* then CH2 disabled */

    RailMonitor mon;
    CHECK(mon.init(bus, 0x40, RailMonitor::Config{RSHUNT, MAX_A}, 20));
    RailMonitor::Sample s{};
    CHECK(mon.poll(1, s, 10));
    CHECK_NEAR(s.bus_v, 3.3, 0.005);
    CHECK_NEAR(s.current_a, 0.5, 0.01);

    /* Re-enable and convert: live data again. */
    dev.setConfig(0x7127);
    dev.setMeasurement(1, 7.5f, 0.0125f);
    CHECK(mon.poll(1, s, 10));
    CHECK_NEAR(s.bus_v, 7.5, 0.005);
    CHECK_NEAR(s.current_a, 2.5, 0.005);
}

/* ---------- item 5: exact bus sequences ---------- */

bool anyBareRead(const MockI2cBus& bus) {
    for (const auto& t : bus.txnLog()) {
        if (t.rx_len > 0 && t.tx_len == 0) return true;
    }
    return false;
}

/* INA226: probe reads 0xFE/0xFF (pointer-write + repeated-start read in one
 * transfer), then a 3-byte CAL write, then a CONFIG read to confirm the MODE
 * bits are continuous (POR 0x4127 already is, so no write-back).  poll reads
 * BUS, CURRENT, POWER.  The written CAL bytes on the wire are asserted
 * MSB-first.  INA226 requires the register-pointer write before each read
 * [V: standard SMBus read-word; the strictness the real part enforces past
 * that is not observable through II2cBus — see report]. */
void test_ina226_bus_sequence() {
    std::printf("[rail-adv] INA226 bus transaction sequence\n");
    MockI2cBus bus;
    Ina226Model dev;
    bus.attach(0x40, &dev);

    RailMonitor mon;
    CHECK(mon.init(bus, 0x40, RailMonitor::Config{RSHUNT, MAX_A}, 20));
    RailMonitor::Sample s{};
    CHECK(mon.poll(0, s, 10));

    const auto& log = bus.txnLog();
    CHECK(log.size() == 7u);
    CHECK(log[0].tx0 == 0xFE && log[0].tx_len == 1 && log[0].rx_len == 2);
    CHECK(log[1].tx0 == 0xFF && log[1].tx_len == 1 && log[1].rx_len == 2);
    CHECK(log[2].tx0 == 0x05 && log[2].tx_len == 3 && log[2].rx_len == 0);
    CHECK(log[2].tx1 == 0x0D && log[2].tx2 == 0x1B);  /* CAL 3355, MSB first */
    CHECK(log[3].tx0 == 0x00 && log[3].tx_len == 1 && log[3].rx_len == 2); /* CONFIG RMW read */
    CHECK(log[4].tx0 == 0x02 && log[4].rx_len == 2);  /* BUS */
    CHECK(log[5].tx0 == 0x04 && log[5].rx_len == 2);  /* CURRENT */
    CHECK(log[6].tx0 == 0x03 && log[6].rx_len == 2);  /* POWER */
    CHECK(!anyBareRead(bus));
}

/* INA228: probe tries the legacy 0xFE/0xFF first (mismatch), then 0x3E/0x3F,
 * writes SHUNT_CAL (1250 = 0x04E2), then reads CONFIG (0x00) to force
 * ADCRANGE=0 — a write-back only happens if the bit was set (covered by
 * test_ina228_stale_adcrange).  poll does 3-byte reads. */
void test_ina228_bus_sequence() {
    std::printf("[rail-adv] INA228 bus transaction sequence\n");
    MockI2cBus bus;
    Ina228Model dev(RSHUNT);
    bus.attach(0x41, &dev);

    RailMonitor mon;
    CHECK(mon.init(bus, 0x41, RailMonitor::Config{RSHUNT, MAX_A}, 20));
    RailMonitor::Sample s{};
    CHECK(mon.poll(0, s, 10));

    const auto& log = bus.txnLog();
    CHECK(log.size() == 9u);
    CHECK(log[0].tx0 == 0xFE && log[0].tx_len == 1 && log[0].rx_len == 2);
    CHECK(log[1].tx0 == 0xFF && log[1].tx_len == 1 && log[1].rx_len == 2);
    CHECK(log[2].tx0 == 0x3E && log[2].tx_len == 1 && log[2].rx_len == 2);
    CHECK(log[3].tx0 == 0x3F && log[3].tx_len == 1 && log[3].rx_len == 2);
    CHECK(log[4].tx0 == 0x02 && log[4].tx_len == 3 && log[4].rx_len == 0);
    CHECK(log[4].tx1 == 0x04 && log[4].tx2 == 0xE2);  /* SHUNT_CAL 1250 = 0x04E2 */
    CHECK(log[5].tx0 == 0x00 && log[5].rx_len == 2);  /* CONFIG RMW read */
    CHECK(log[6].tx0 == 0x05 && log[6].rx_len == 3);  /* VBUS    24-bit */
    CHECK(log[7].tx0 == 0x07 && log[7].rx_len == 3);  /* CURRENT 24-bit */
    CHECK(log[8].tx0 == 0x08 && log[8].rx_len == 3);  /* POWER   24-bit */
    /* CONFIG was at POR (ADCRANGE=0), so the RMW must NOT write it back. */
    bool wrote_config = false;
    for (const auto& t : log) {
        if (t.tx0 == 0x00 && t.tx_len == 3) wrote_config = true;
    }
    CHECK(!wrote_config);
    CHECK(!anyBareRead(bus));
}

/* INA3221 poll order: 2-byte read of the BUS register first, then the SHUNT
 * register, per channel (channel n: 0x02+2n then 0x01+2n).  init() adds a
 * CONFIG (0x00) read-modify-write after the ID probe: CH_EN[14:12] all set +
 * MODE[2:0]=111; with the POR value 0x7127 both hold, so no write-back.  [D] */
void test_ina3221_bus_sequence() {
    std::printf("[rail-adv] INA3221 bus transaction sequence\n");
    MockI2cBus bus;
    Ina3221Model dev;
    bus.attach(0x40, &dev);
    dev.setMeasurement(1, 3.3f, 0.0025f);

    RailMonitor mon;
    CHECK(mon.init(bus, 0x40, RailMonitor::Config{RSHUNT, MAX_A}, 20));
    RailMonitor::Sample s{};
    CHECK(mon.poll(1, s, 10));

    const auto& log = bus.txnLog();
    CHECK(log.size() == 5u);
    CHECK(log[0].tx0 == 0xFE && log[0].tx_len == 1 && log[0].rx_len == 2);
    CHECK(log[1].tx0 == 0xFF && log[1].tx_len == 1 && log[1].rx_len == 2);
    CHECK(log[2].tx0 == 0x00 && log[2].tx_len == 1 && log[2].rx_len == 2); /* CONFIG RMW read */
    CHECK(log[3].tx0 == 0x04 && log[3].tx_len == 1 && log[3].rx_len == 2);
    CHECK(log[4].tx0 == 0x03 && log[4].tx_len == 1 && log[4].rx_len == 2);
    CHECK(!anyBareRead(bus));
}
} // namespace

void hosttest::test_rail_adv_suite() {
    test_ina226_cal_worked_example();
    test_ina226_end_to_end_uneven_cal();
    test_ina226_overrange_wraps_negative();
    test_ina226_shunt_raw_min_negative();
    test_ina226_cal_clamps();
    test_ina228_20bit_boundaries();
    test_ina228_large_power_24bit();
    test_ina228_stale_adcrange();
    test_die_id_revision_masking();
    test_ina3221_disabled_channel_reads();
    test_ina3221_disabled_channel_stale();
    test_ina226_bus_sequence();
    test_ina228_bus_sequence();
    test_ina3221_bus_sequence();
}
