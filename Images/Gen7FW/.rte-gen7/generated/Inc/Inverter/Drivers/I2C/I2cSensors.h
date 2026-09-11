#pragma once

#include "Inverter/Drivers/I2C/HalI2cBus.h"
#include "Inverter/Drivers/I2C/OnboardTempSensor.h"
#include "Inverter/Drivers/I2C/RailMonitor.h"

#include <cstdint>
#include <cmath>

namespace Inverter {

/**
 * @brief Application-level owner of the Gen7 I2C sensors.
 *
 * Owns the two bus objects (I2C5 = on-board temperature, I2C4 = rail
 * monitor), the configurable parameters, the poll/re-probe scheduling, and
 * the telemetry + warning-fault wiring.  Follows the ApplicationSensors
 * pattern: init() never blocks boot (short-timeout probe, one log line when
 * absent), update() runs from the main loop only (never from an ISR) at
 * POLL_MS cadence, and absent devices are lazily re-probed every
 * REPROBE_MS.
 *
 * Persistent config (RteParamStore KV, `config set <key> <value>` +
 * `config saveall`):
 *   OnbTemp.Addr      7-bit temp-sensor address   (default 0x48)
 *   OnbTemp.CritC     over-temperature warning     (default 85 degC)
 *   RailMon.Addr      7-bit rail-monitor address  (default 0x40)
 *   RailMon.ShuntOhm  shunt resistor [ohm]        (default 0.005)
 *   RailMon.MaxA      max expected current [A]    (default 10)
 *   RailMon.OvV       rail OV warning [V]         (default 0 = disabled)
 *   RailMon.UvV       rail UV warning [V]         (default 0 = disabled)
 *
 * Telemetry keys: onb_temp_c, rail_v, rail_a, rail_w (plus rail2_v/rail2_a
 * and rail3_v/rail3_a for the INA3221's extra channels).
 *
 * Warning faults (latched, log-only): OnboardOvertemperature,
 * RailOvervoltage, RailUndervoltage.
 */
class I2cSensors {
public:
    bool init();
    void update();
    void reloadConfig();

    /* Status for the shell commands. */
    bool        tempPresent() const    { return m_temp.part() != OnboardTempSensor::Part::None; }
    float       lastTempC() const      { return m_temp_c; }
    float       tempCritC() const      { return m_temp_crit_c; }
    const OnboardTempSensor& tempSensor() const { return m_temp; }

    bool        railPresent() const    { return m_rail.part() != RailMonitor::Part::None; }
    const RailMonitor& railMonitor() const { return m_rail; }
    const RailMonitor::Sample& railSample(uint8_t ch) const { return m_rail_sample[ch < RailMonitor::MAX_CHANNELS ? ch : 0]; }

    HalI2cBus& tempBus() { return m_bus_temp; }
    HalI2cBus& railBus() { return m_bus_rail; }

    /* Scan a bus for the `i2cscan` shell command; counts ACKed addresses in
     * 0x08..0x77 into out[0..119].  Returns the number found. */
    uint32_t scan(uint8_t bus, uint8_t* out_addrs, uint32_t max_out);

private:
    static constexpr uint32_t POLL_MS       = 200;   /**< 5 Hz              */
    static constexpr uint32_t REPROBE_MS    = 5000;  /**< lazy re-probe     */
    static constexpr uint32_t PROBE_TIMEOUT_MS = 20; /**< boot-blocking cap */
    static constexpr uint32_t POLL_TIMEOUT_MS  = 10;
    static constexpr uint32_t FAULT_SUSTAIN_MS = 500;
    static constexpr float    HYST_C = 5.0f;
    static constexpr float    HYST_V = 0.5f;
    static constexpr uint8_t  MAX_POLL_ERRORS = 3;   /**< -> mark absent    */

    bool probeTemp();
    bool probeRail();
    void pollTemp(uint32_t now_ms);
    void pollRail(uint32_t now_ms);
    void evaluateFaults(uint32_t now_ms);

    HalI2cBus         m_bus_temp{&hi2c5};   /**< onboard temp bus  */
    HalI2cBus         m_bus_rail{&hi2c4};   /**< rail monitor bus  */
    OnboardTempSensor m_temp;
    RailMonitor       m_rail;

    float m_temp_c = NAN;
    RailMonitor::Sample m_rail_sample[RailMonitor::MAX_CHANNELS] = {};

    uint8_t  m_temp_addr  = 0x48;
    float    m_temp_crit_c = 85.0f;
    uint8_t  m_rail_addr  = 0x40;
    float    m_shunt_ohm  = 0.005f;
    float    m_rail_max_a = 10.0f;
    float    m_rail_ov_v  = 0.0f;   /**< 0 = disabled */
    float    m_rail_uv_v  = 0.0f;   /**< 0 = disabled */

    uint8_t  m_temp_errors = 0;
    uint8_t  m_rail_errors = 0;
    uint32_t m_last_poll_ms    = 0;
    uint32_t m_temp_reprobe_ms = 0;
    uint32_t m_rail_reprobe_ms = 0;

    bool     m_ot_cond = false, m_ot_raised = false;
    uint32_t m_ot_since_ms = 0;
    bool     m_ov_cond = false, m_ov_raised = false;
    uint32_t m_ov_since_ms = 0;
    bool     m_uv_cond = false, m_uv_raised = false;
    uint32_t m_uv_since_ms = 0;

    bool     m_initialized = false;
};

/** @brief Global I2C sensors instance. */
I2cSensors& i2cSensors();

} // namespace Inverter
