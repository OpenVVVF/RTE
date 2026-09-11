/* Adversarial coverage for OnboardTempSensor (TMP1075/TMP102/PCT2075 path).
 *
 * Focus: incorrect-but-self-consistent behavior the happy-path tests would
 * NOT catch — 12/13-bit boundary patterns, low-LSB discard, EM/data-width
 * mismatches, pointer-register aliasing on generic parts, reserved-pointer
 * NAKs, byte order on the wire, and exact bus transaction shapes.
 *
 * Provenance tags:
 *   [V] verified against a public datasheet-derived source (Linux kernel
 *       drivers, vendor library notes) during this pass
 *   [A] assumed / mock-informed (mock encodes the same assumption as the
 *       driver author; could not be re-verified against datasheet text)
 *   [D] documented driver behavior (characterized, not necessarily a bug)
 */
#include "test_framework.h"
#include "mock_i2c.h"

#include "Inverter/Drivers/I2C/OnboardTempSensor.h"

#include <cstdint>
#include <map>

using Inverter::OnboardTempSensor;
using hosttest::MockI2cBus;
using hosttest::Tmp1075Model;

namespace {

constexpr uint8_t ADDR = 0x48;

/* Serves hard-coded wire bytes per pointer, like a logic-analyzer capture. */
class TraceDevice : public hosttest::MockDevice {
public:
    void serve(uint8_t ptr, uint8_t b0, uint8_t b1) {
        m_bytes[ptr] = (static_cast<uint16_t>(b0) << 8) | b1;
    }
    bool onWrite(const uint8_t* data, uint8_t /*len*/) override {
        m_ptr = data[0];
        return true;
    }
    void onRead(uint8_t* out, uint8_t len) override {
        const uint16_t v = m_bytes.count(m_ptr) ? m_bytes[m_ptr] : 0;
        if (len >= 1) out[0] = static_cast<uint8_t>(v >> 8);
        if (len >= 2) out[1] = static_cast<uint8_t>(v & 0xFF);
        for (uint8_t i = 2; i < len; ++i) out[i] = 0;
    }
private:
    std::map<uint8_t, uint16_t> m_bytes;
    uint8_t m_ptr = 0;
};

/* 12-bit boundary/odd-LSB decode vectors — computed independently of both
 * mock and driver (hand arithmetic on the two's-complement left-aligned
 * format).  Table rows marked [A] additionally appear in the TMP102
 * datasheet temperature table; the PDF was not fetchable in this session. */
void test_decode_12bit_boundaries() {
    std::printf("[temp-adv] 12-bit boundary decode\n");
    /* 0x9600 == +150 degC is NOT representable in 12-bit (code 2400 > 2047
     * wraps to -1696); +150 requires EM=1.  [A: TMP102 table] */
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x9600, false), -106.0, 1e-4);
    /* Datasheet table row: -55 degC -> 1100 1001 0000 0000.  [A] */
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0xC900, false), -55.0, 1e-4);
    /* Smallest negative step: code -1, and with garbage low nibble (0xFFFF
     * must still be -0.0625, not +anything — low 4 bits are not data). */
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0xFFF0, false), -0.0625, 1e-6);
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0xFFFF, false), -0.0625, 1e-6);
    /* 0x800F: bits below the 12-bit field must be DISCARDED, not rounded —
     * SAR must give -128.0, never -127.9375. */
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x800F, false), -128.0, 1e-4);
    /* Odd LSB patterns inside the ignored nibble must not change the code. */
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x190F, false), 25.0, 1e-4);
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x1900, false), 25.0, 1e-4);
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x19C0, false), 25.75, 1e-4);
}

/* EM (13-bit) boundary vectors and mode/data-width mismatches. */
void test_decode_13bit_boundaries() {
    std::printf("[temp-adv] 13-bit (EM) boundary decode\n");
    /* +150 degC max in EM: code 2400 -> 2400<<3 = 0x4B00.  [A: TMP102 table] */
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x4B00, true), 150.0, 1e-4);
    /* -55 degC in EM: -880 -> 0xC90<<3 = 0xE480. */
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0xE480, true), -55.0, 1e-4);
    /* 13-bit span extremes. */
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x8000, true), -256.0, 1e-4);
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0xFFFF, true), -0.0625, 1e-6);

    /* EM=1 applied to a 12-bit-formatted pattern (0x1900 = 25 degC 12-bit)
     * DOUBLES the reading -> 50.0.  [D: why the driver latches EM at init] */
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x1900, true), 50.0, 1e-4);
    /* EM=0 applied to 13-bit-formatted data (150 degC = 0x4B00) HALVES it. */
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x4B00, false), 75.0, 1e-4);
}

/* The EM extractor itself is mode-blind; per-part policy lives in init().
 * 0x00FF is the real TMP1075 config POR (SBOS854F Table 7-5 [V]) — bit 4 set. */
void test_em_bit_extraction_corners() {
    std::printf("[temp-adv] config EM-bit corners\n");
    CHECK(OnboardTempSensor::configIsExtended(0xFFFF));
    CHECK(OnboardTempSensor::configIsExtended(0x00F0));
    CHECK(!OnboardTempSensor::configIsExtended(0x00EF));
    CHECK(OnboardTempSensor::configIsExtended(0x00FF));  /* TMP1075 POR: ignored
        by init() only because the part is positively ID'd — see test_temp.cpp
        test_tmp1075_config_por_is_not_em. */
}

/* encode/decode round trips in both widths, incl. values only EM can hold.
 * Plus: encoding +150 degC in 12-bit PRODUCES the ambiguous 0x9600 — the
 * reg<->degC relation is not round-trippable at the boundary. [D] */
void test_encode_decode_boundaries() {
    std::printf("[temp-adv] encode/decode boundary round trips\n");
    const struct { float t; bool ext; } cases[] = {
        {150.0f, true}, {-256.0f, true}, {-55.4375f, false}, {127.9375f, false},
    };
    for (const auto& c : cases) {
        const uint16_t reg = OnboardTempSensor::encodeTempReg(c.t, c.ext);
        CHECK_NEAR(OnboardTempSensor::decodeTempC(reg, c.ext), c.t, 1e-4);
    }
    CHECK(OnboardTempSensor::encodeTempReg(150.0f, false) == 0x9600u);
    CHECK_NEAR(OnboardTempSensor::decodeTempC(
                   OnboardTempSensor::encodeTempReg(150.0f, false), false),
               -106.0, 1e-4);
}

/* TMP102-class generic part: pointer register masks to P1:P0, so the driver's
 * DEVICE_ID probe at 0x0F lands on THIGH (POR 0x5000), not 0x0000.  [A on the
 * masking; SBOS397 pointer figure says only P1/P0 are used.]  Either way the
 * driver must classify Generic and convert normally. */
void test_generic_pointer_aliasing() {
    std::printf("[temp-adv] TMP102 probe-at-0x0F aliases THIGH\n");
    MockI2cBus bus;
    Tmp1075Model dev(false);  /* generic = TMP102 flavor, 2-bit pointer */
    bus.attach(ADDR, &dev);

    /* Direct read at pointer 0x0F returns THIGH bytes, not 0x0000: */
    uint8_t out[2] = {0, 0};
    const uint8_t ptr = 0x0F;
    CHECK(bus.transfer(ADDR, &ptr, 1, out, 2, 10));
    CHECK(out[0] == 0x50 && out[1] == 0x00);

    OnboardTempSensor sensor;
    CHECK(sensor.init(bus, ADDR, 20));
    CHECK(sensor.part() == OnboardTempSensor::Part::Generic);  /* 0x5000 != 0x7500 */
    CHECK(!sensor.extendedMode());  /* TMP102 config POR 0x60A0 -> EM=0 */
    dev.setTempRaw16(0x4B00);       /* 75 degC, 12-bit */
    float t = 0.0f;
    CHECK(sensor.poll(t, 10));
    CHECK_NEAR(t, 75.0, 1e-3);
}

/* A stricter generic part may NAK the read phase at an unimplemented pointer
 * (PCT2075-style reserved pointer).  [A: datasheet silent; chosen mock
 * behavior].  init() must survive and still classify Generic. */
void test_generic_nak_on_id_probe() {
    std::printf("[temp-adv] generic part NAKs reserved-pointer reads\n");
    MockI2cBus bus;
    Tmp1075Model dev(false);
    dev.nakReadPointer(0x0F);  /* masked to 0x03 in the model */
    bus.attach(ADDR, &dev);

    OnboardTempSensor sensor;
    CHECK(sensor.init(bus, ADDR, 20));
    CHECK(sensor.part() == OnboardTempSensor::Part::Generic);
    dev.setTempRaw16(0x1900);
    float t = 0.0f;
    CHECK(sensor.poll(t, 10));
    CHECK_NEAR(t, 25.0, 1e-3);
}

/* Config register unreadable (NAK) on a generic part whose ADC is in EM:
 * init() must not fail, must leave EM=0, and poll() then decodes the
 * 13-bit-formatted data as 12-bit (150 degC hardware reads 75 degC).
 * [D: characterized fallback; mismatched-width data is a hardware config
 * error the driver cannot detect from the temp register alone.] */
void test_config_read_failure_falls_back_to_12bit() {
    std::printf("[temp-adv] config NAK -> 12-bit fallback\n");
    MockI2cBus bus;
    Tmp1075Model dev(false);
    dev.nakReadPointer(0x01);
    dev.setTempRaw16(0x4B00);  /* 150 degC if EM were latched */
    bus.attach(ADDR, &dev);

    OnboardTempSensor sensor;
    CHECK(sensor.init(bus, ADDR, 20));
    CHECK(sensor.part() == OnboardTempSensor::Part::Generic);
    CHECK(!sensor.extendedMode());
    float t = 0.0f;
    CHECK(sensor.poll(t, 10));
    CHECK_NEAR(t, 75.0, 1e-3);
}

/* Real wire bytes are MSB-first (I2C convention; INA226 datasheet quote
 * "All data bytes are transmitted most significant byte first" [V via
 * third-party library docs]).  Serve an analyzer-style capture and check the
 * driver decodes MSB-first — and characterize what a byte-swapped capture
 * (wiring/tooling bug) would silently produce. */
void test_wire_byte_order() {
    std::printf("[temp-adv] wire byte order (MSB first)\n");
    MockI2cBus bus;
    TraceDevice dev;
    dev.serve(0x0F, 0x75, 0x00);  /* DEVICE_ID -> TMP1075 */
    dev.serve(0x00, 0x19, 0x80);  /* 25.5 degC, MSB first */
    bus.attach(ADDR, &dev);

    OnboardTempSensor sensor;
    CHECK(sensor.init(bus, ADDR, 20));
    CHECK(sensor.part() == OnboardTempSensor::Part::Tmp1075);
    float t = 0.0f;
    CHECK(sensor.poll(t, 10));
    CHECK_NEAR(t, 25.5, 1e-3);

    /* Byte-swapped capture of the same register: 0x8019 -> -127.9375 degC.
     * The decode direction is pinned by this test; nothing in the driver can
     * detect a swapped physical bus. [D] */
    dev.serve(0x00, 0x80, 0x19);
    CHECK(sensor.poll(t, 10));
    CHECK_NEAR(t, -127.9375, 1e-3);
}

/* Exact transaction shapes.  Every read is a 1-byte pointer write + N-byte
 * read within ONE transfer (the II2cBus contract for "pointer write,
 * repeated START, read"; HalI2cBus realizes it with HAL_I2C_Mem_Read).
 * A STOP-separated sequence is not expressible through II2cBus, so asserting
 * (tx_len==1 whenever rx_len>0) covers the strict part of the requirement.
 * [V: read-only inspection of HalI2cBus.cpp] */
void test_transaction_shapes() {
    std::printf("[temp-adv] transaction shapes\n");
    MockI2cBus bus;
    Tmp1075Model dev(true);
    bus.attach(ADDR, &dev);

    OnboardTempSensor sensor;
    CHECK(sensor.init(bus, ADDR, 20));
    float t = 0.0f;
    CHECK(sensor.poll(t, 10));

    const auto& log = bus.txnLog();
    /* TMP1075: init reads ONLY DEVICE_ID (config read skipped by the EM fix),
     * poll reads the temp register -> exactly 2 transfers. */
    CHECK(log.size() == 2u);
    CHECK(log[0].tx0 == 0x0F && log[0].tx_len == 1 && log[0].rx_len == 2);
    CHECK(log[1].tx0 == 0x00 && log[1].tx_len == 1 && log[1].rx_len == 2);
    for (const auto& txn : log) {
        CHECK(txn.ok);
        CHECK(!(txn.rx_len > 0 && txn.tx_len == 0));  /* never a bare read */
    }

    /* Generic flavor also reads CONFIG during init -> 3 transfers. */
    MockI2cBus bus2;
    Tmp1075Model dev2(false);
    bus2.attach(ADDR, &dev2);
    OnboardTempSensor s2;
    CHECK(s2.init(bus2, ADDR, 20));
    const auto& log2 = bus2.txnLog();
    CHECK(log2.size() == 2u);
    CHECK(log2[0].tx0 == 0x0F && log2[1].tx0 == 0x01);
    CHECK(log2[0].tx_len == 1 && log2[0].rx_len == 2);
}

} // namespace

void hosttest::test_temp_adv_suite() {
    test_decode_12bit_boundaries();
    test_decode_13bit_boundaries();
    test_em_bit_extraction_corners();
    test_encode_decode_boundaries();
    test_generic_pointer_aliasing();
    test_generic_nak_on_id_probe();
    test_config_read_failure_falls_back_to_12bit();
    test_wire_byte_order();
    test_transaction_shapes();
}
