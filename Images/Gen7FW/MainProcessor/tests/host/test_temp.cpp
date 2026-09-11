#include "test_framework.h"
#include "mock_i2c.h"

#include "Inverter/Drivers/I2C/OnboardTempSensor.h"

using Inverter::OnboardTempSensor;
using hosttest::MockI2cBus;
using hosttest::Tmp1075Model;

namespace {

constexpr uint8_t ADDR = 0x48;

/* Pure decode math: TMP102-family register -> degC. */
void test_decode_vectors() {
    std::printf("[temp] register decode vectors\n");
    /* 12-bit normal mode: code = reg >> 4 (arithmetic), 0.0625 degC/LSB. */
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x7FF0, false), 127.9375, 1e-4);
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x1900, false), 25.0, 1e-4);
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x0000, false), 0.0, 1e-6);
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0xFF10, false), -0.9375, 1e-4);
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0xF900, false), -7.0, 1e-4);
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x9000, false), -112.0, 1e-4);
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x8000, false), -128.0, 1e-4);

    /* 13-bit extended mode (EM=1): code = reg >> 3 (arithmetic). */
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x0C80, true), 25.0, 1e-4);
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0x7FF8, true), 255.9375, 1e-4);
    CHECK_NEAR(OnboardTempSensor::decodeTempC(0xF380, true), -25.0, 1e-4);

    /* EM bit extraction from a config word (EM = bit 4 of the low byte). */
    CHECK(!OnboardTempSensor::configIsExtended(0x0000));
    CHECK(OnboardTempSensor::configIsExtended(0x0010));
    CHECK(OnboardTempSensor::configIsExtended(0x0050));  /* CR1|EM */
    CHECK(!OnboardTempSensor::configIsExtended(0x1040)); /* high-byte bits */
}

/* Encode round-trip used by the mock to program temperatures. */
void test_encode_roundtrip() {
    std::printf("[temp] encode/decode round-trip\n");
    const float temps[] = {-40.0f, -7.0f, 0.0f, 25.0f, 85.0f, 127.9f};
    for (float t : temps) {
        const uint16_t reg = OnboardTempSensor::encodeTempReg(t, false);
        CHECK_NEAR(OnboardTempSensor::decodeTempC(reg, false), t, 0.0625);
    }
}

/* TMP1075 on the bus: detected, identified by DEVICE_ID 0x7500, converts. */
void test_tmp1075_present() {
    std::printf("[temp] TMP1075 present at 0x48\n");
    MockI2cBus bus;
    Tmp1075Model dev(true);
    bus.attach(ADDR, &dev);

    OnboardTempSensor sensor;
    CHECK(sensor.init(bus, ADDR, 20));
    CHECK(sensor.part() == OnboardTempSensor::Part::Tmp1075);
    CHECK(sensor.address() == ADDR);
    CHECK(!sensor.extendedMode());

    dev.setTempRaw16(OnboardTempSensor::encodeTempReg(25.0f, false));
    float t = 0.0f;
    CHECK(sensor.poll(t, 10));
    CHECK_NEAR(t, 25.0, 1e-3);

    /* Negative temperature exercise. */
    dev.setTempRaw16(OnboardTempSensor::encodeTempReg(-7.0f, false));
    CHECK(sensor.poll(t, 10));
    CHECK_NEAR(t, -7.0, 1e-3);
    /* -40 C (cold-start corner). */
    dev.setTempRaw16(OnboardTempSensor::encodeTempReg(-40.0f, false));
    CHECK(sensor.poll(t, 10));
    CHECK_NEAR(t, -40.0, 0.07);
}

/* No DEVICE_ID register -> generic family member, still converts. */
void test_generic_present() {
    std::printf("[temp] generic (TMP102/PCT2075-style) device at 0x49\n");
    MockI2cBus bus;
    Tmp1075Model dev(false);
    bus.attach(0x49, &dev);

    OnboardTempSensor sensor;
    CHECK(sensor.init(bus, 0x49, 20));
    CHECK(sensor.part() == OnboardTempSensor::Part::Generic);

    dev.setTempRaw16(OnboardTempSensor::encodeTempReg(55.5f, false));
    float t = 0.0f;
    CHECK(sensor.poll(t, 10));
    CHECK_NEAR(t, 55.5, 0.07);
}

/* EM=1 in config -> 13-bit decode on the wire.  EM is a TMP102-class-only
 * feature: the real TMP1075 has no EM bit and its config PORs to 0x00FF, so
 * this test must use the generic (TMP102-flavored) model. */
void test_extended_mode() {
    std::printf("[temp] extended (13-bit) mode, generic TMP102 flavor\n");
    MockI2cBus bus;
    Tmp1075Model dev(false);
    bus.attach(ADDR, &dev);
    dev.setConfig(0x0010);  /* EM=1 */
    dev.setTempRaw16(0x0C80); /* 25.0 C in 13-bit mode */

    OnboardTempSensor sensor;
    CHECK(sensor.init(bus, ADDR, 20));
    CHECK(sensor.part() == OnboardTempSensor::Part::Generic);
    CHECK(sensor.extendedMode());

    float t = 0.0f;
    CHECK(sensor.poll(t, 10));
    CHECK_NEAR(t, 25.0, 1e-3);
}

/* Real TMP1075: config PORs to 0x00FF with every "Not used" low bit reading
 * 1 (SBOS854F Table 7-5), so an unconditional EM-bit (bit 4) check would
 * latch a bogus 13-bit decode and report 2x the real temperature.  The
 * driver must ignore EM whenever the part was ID'd as TMP1075. */
void test_tmp1075_config_por_is_not_em() {
    std::printf("[temp] TMP1075 config POR 0x00FF must not engage EM\n");
    MockI2cBus bus;
    Tmp1075Model dev(true);
    bus.attach(ADDR, &dev);
    CHECK(dev.config() == 0x00FFu);
    dev.setTempRaw16(0x1900);  /* 25.0 C (12-bit is the only TMP1075 width) */

    OnboardTempSensor sensor;
    CHECK(sensor.init(bus, ADDR, 20));
    CHECK(sensor.part() == OnboardTempSensor::Part::Tmp1075);
    CHECK(!sensor.extendedMode());  /* bit 4 of 0x00FF is 1: must be ignored */
    float t = 0.0f;
    CHECK(sensor.poll(t, 10));
    CHECK_NEAR(t, 25.0, 1e-3);      /* 2x decode would read 50.0 C */
}

/* NACK at the address: init must fail cleanly, poll must not read. */
void test_absent() {
    std::printf("[temp] absent device (NACK)\n");
    MockI2cBus bus;
    OnboardTempSensor sensor;
    CHECK(!sensor.init(bus, ADDR, 20));
    CHECK(sensor.part() == OnboardTempSensor::Part::None);
    float t = 123.0f;
    CHECK(!sensor.poll(t, 10));
    CHECK_NEAR(t, 123.0, 1e-6);  /* output untouched on failure */
    /* poll() on an uninitialised sensor must not touch the bus. */
    OnboardTempSensor fresh;
    CHECK(!fresh.poll(t, 10));
    /* The absent-probe used isReady only; no register transfers happened. */
    CHECK(bus.transferCount() == 0);
}

/* Device vanishes after a successful init (connector pulled): subsequent
 * polls NACK and must fail cleanly with the output untouched. */
void test_loss_of_device() {
    std::printf("[temp] device lost after init\n");
    MockI2cBus bus;
    Tmp1075Model dev(true);
    bus.attach(ADDR, &dev);

    OnboardTempSensor sensor;
    CHECK(sensor.init(bus, ADDR, 20));
    float t = 0.0f;
    CHECK(sensor.poll(t, 10));

    bus.detach(ADDR);
    float t2 = 123.0f;
    CHECK(!sensor.poll(t2, 10));
    CHECK_NEAR(t2, 123.0, 1e-6);  /* failure leaves the output untouched */
}

} // namespace

void hosttest::test_temp_suite() {
    test_decode_vectors();
    test_encode_roundtrip();
    test_tmp1075_present();
    test_generic_present();
    test_extended_mode();
    test_tmp1075_config_por_is_not_em();
    test_absent();
    test_loss_of_device();
}
