#include "Inverter/Drivers/I2C/I2cSensors.h"

#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Telemetry.h"

#include "main.h"
#include "i2c.h"

namespace Inverter {

namespace {
I2cSensors s_instance;
} // namespace

I2cSensors& i2cSensors() {
    return s_instance;
}

namespace {
/* KV load mirror of ApplicationSensors::loadConfig: defaults are persisted
 * on first boot so `config list` shows the tunables. */
float loadOne(const char* key, float def, bool persist_defaults) {
    float value = def;
    if (!RteParamStore::isReady()) {
        return def;
    }
    if (!RteParamStore::get(key, &value) && persist_defaults) {
        RteParamStore::set(key, def);
        value = def;
    }
    return value;
}
} // namespace

bool I2cSensors::init() {
    m_temp_c = NAN;
    for (uint8_t i = 0; i < RailMonitor::MAX_CHANNELS; ++i) {
        m_rail_sample[i] = {NAN, NAN, NAN};
    }

    const bool persist = true;
    const float addr_t = loadOne("OnbTemp.Addr", 0x48, persist);
    m_temp_crit_c = loadOne("OnbTemp.CritC", 85.0f, persist);
    const float addr_r = loadOne("RailMon.Addr", 0x40, persist);
    m_shunt_ohm  = loadOne("RailMon.ShuntOhm", 0.005f, persist);
    m_rail_max_a = loadOne("RailMon.MaxA", 10.0f, persist);
    m_rail_ov_v  = loadOne("RailMon.OvV", 0.0f, persist);
    m_rail_uv_v  = loadOne("RailMon.UvV", 0.0f, persist);
    if (persist && RteParamStore::isReady()) {
        RteParamStore::flush();
    }

    /* Clamp addresses to valid 7-bit range. */
    m_temp_addr = (addr_t >= 8.0f && addr_t <= 119.0f)
                      ? static_cast<uint8_t>(addr_t) : 0x48;
    m_rail_addr = (addr_r >= 8.0f && addr_r <= 119.0f)
                      ? static_cast<uint8_t>(addr_r) : 0x40;

    const bool t_ok = probeTemp();
    const bool r_ok = probeRail();
    Telemetry::printf("[I2C] onb_temp: %s @ I2C5/0x%02X",
                      t_ok ? m_temp.partName() : "absent", m_temp_addr);
    Telemetry::printf("[I2C] rail_mon: %s @ I2C4/0x%02X (Rsh=%.3f ohm, Imax=%.1f A)",
                      r_ok ? m_rail.partName() : "absent", m_rail_addr,
                      static_cast<double>(m_shunt_ohm),
                      static_cast<double>(m_rail_max_a));

    m_initialized = true;
    const uint32_t now = HAL_GetTick();
    m_last_poll_ms = now;
    return t_ok || r_ok;
}

bool I2cSensors::probeTemp() {
    m_temp_errors = 0;
    const bool ok = m_temp.init(m_bus_temp, m_temp_addr, PROBE_TIMEOUT_MS);
    if (!ok) {
        /* Absent devices are re-probed lazily from update(). */
        m_temp_reprobe_ms = HAL_GetTick();
    }
    return ok;
}

bool I2cSensors::probeRail() {
    m_rail_errors = 0;
    const RailMonitor::Config cfg{m_shunt_ohm, m_rail_max_a};
    const bool ok = m_rail.init(m_bus_rail, m_rail_addr, cfg, PROBE_TIMEOUT_MS);
    if (!ok) {
        m_rail_reprobe_ms = HAL_GetTick();
    }
    return ok;
}

void I2cSensors::pollTemp(uint32_t now_ms) {
    (void)now_ms;
    float t = NAN;
    if (m_temp.poll(t, POLL_TIMEOUT_MS)) {
        m_temp_errors = 0;
        if (std::isfinite(t)) {
            m_temp_c = t;
            Telemetry::log("onb_temp_c", t);
        }
        return;
    }
    if (++m_temp_errors >= MAX_POLL_ERRORS) {
        Telemetry::printf("[I2C] onb_temp lost @ 0x%02X, re-arming probe", m_temp_addr);
        m_temp.init(m_bus_temp, m_temp_addr, PROBE_TIMEOUT_MS);  /* resets part to None */
        m_temp_reprobe_ms = now_ms;
    }
}

void I2cSensors::pollRail(uint32_t now_ms) {
    bool ok = true;
    const uint8_t nch = m_rail.channels();
    for (uint8_t ch = 0; ch < nch && ch < RailMonitor::MAX_CHANNELS; ++ch) {
        RailMonitor::Sample s{};
        if (m_rail.poll(ch, s, POLL_TIMEOUT_MS)) {
            m_rail_sample[ch] = s;
        } else {
            ok = false;
        }
    }
    if (ok && nch > 0) {
        m_rail_errors = 0;
        Telemetry::log("rail_v", m_rail_sample[0].bus_v);
        Telemetry::log("rail_a", m_rail_sample[0].current_a);
        Telemetry::log("rail_w", m_rail_sample[0].power_w);
        if (nch > 1) {
            Telemetry::log("rail2_v", m_rail_sample[1].bus_v);
            Telemetry::log("rail2_a", m_rail_sample[1].current_a);
            Telemetry::log("rail3_v", m_rail_sample[2].bus_v);
            Telemetry::log("rail3_a", m_rail_sample[2].current_a);
        }
        return;
    }
    if (nch > 0 && ++m_rail_errors >= MAX_POLL_ERRORS) {
        Telemetry::printf("[I2C] rail_mon lost @ 0x%02X, re-arming probe", m_rail_addr);
        m_rail.init(m_bus_rail, m_rail_addr, {m_shunt_ohm, m_rail_max_a},
                    PROBE_TIMEOUT_MS);  /* resets part to None */
        m_rail_reprobe_ms = now_ms;
    }
}

void I2cSensors::evaluateFaults(uint32_t now_ms) {
    /* Over-temperature: 5 degC hysteresis, 500 ms sustain, Warning fault. */
    if (tempPresent() && std::isfinite(m_temp_c)) {
        const bool cond = m_ot_cond ? (m_temp_c > (m_temp_crit_c - HYST_C))
                                    : (m_temp_c > m_temp_crit_c);
        if (cond && !m_ot_cond) {
            m_ot_since_ms = now_ms;
        }
        m_ot_cond = cond;
        if (!cond) {
            m_ot_raised = false;
        } else if ((!m_ot_raised ||
                    !FaultManager::instance().isActive(FaultSource::OnboardOvertemperature)) &&
                   (now_ms - m_ot_since_ms) >= FAULT_SUSTAIN_MS) {
            m_ot_raised = true;
            FaultManager::instance().raise(FaultSource::OnboardOvertemperature,
                                           FaultReason::OnboardOvertemperature);
        }
    } else {
        m_ot_cond = false;
        m_ot_raised = false;
    }

    /* Rail OV/UV: only when the threshold is enabled (> 0) and the channel is
     * producing finite values.  0.5 V hysteresis, 500 ms sustain. */
    const float v = m_rail_sample[0].bus_v;
    const bool v_ok = railPresent() && std::isfinite(v);

    auto eval = [&now_ms, this](float threshold, bool& cond, bool& raised,
                                uint32_t& since, FaultSource src, FaultReason reason,
                                bool above, float val, bool enabled) {
        if (!enabled) {
            cond = false;
            raised = false;
            return;
        }
        const bool c = cond ? (above ? val > (threshold - HYST_V)
                                     : val < (threshold + HYST_V))
                            : (above ? val > threshold : val < threshold);
        if (c && !cond) {
            since = now_ms;
        }
        cond = c;
        if (!c) {
            raised = false;
        } else if ((!raised || !FaultManager::instance().isActive(src)) &&
                   (now_ms - since) >= FAULT_SUSTAIN_MS) {
            raised = true;
            FaultManager::instance().raise(src, reason);
        }
    };

    eval(m_rail_ov_v, m_ov_cond, m_ov_raised, m_ov_since_ms,
         FaultSource::RailOvervoltage, FaultReason::RailOvervoltage,
         true, v, v_ok && m_rail_ov_v > 0.0f);
    eval(m_rail_uv_v, m_uv_cond, m_uv_raised, m_uv_since_ms,
         FaultSource::RailUndervoltage, FaultReason::RailUndervoltage,
         false, v, v_ok && m_rail_uv_v > 0.0f);
}

uint32_t I2cSensors::scan(uint8_t bus, uint8_t* out_addrs, uint32_t max_out) {
    HalI2cBus* b = (bus == 5) ? &m_bus_temp : (bus == 4) ? &m_bus_rail : nullptr;
    if (b == nullptr || out_addrs == nullptr) {
        return 0;
    }
    uint32_t found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        if (b->isReady(addr, 2)) {
            if (found < max_out) {
                out_addrs[found++] = addr;
            }
        }
    }
    return found;
}

void I2cSensors::reloadConfig() {
    /* Re-read KV without persisting (values may be shell-set), then re-probe
     * both devices against the (possibly new) addresses/calibration. */
    const bool persist = false;
    const float addr_t = loadOne("OnbTemp.Addr", 0x48, persist);
    m_temp_crit_c = loadOne("OnbTemp.CritC", 85.0f, persist);
    const float addr_r = loadOne("RailMon.Addr", 0x40, persist);
    m_shunt_ohm  = loadOne("RailMon.ShuntOhm", 0.005f, persist);
    m_rail_max_a = loadOne("RailMon.MaxA", 10.0f, persist);
    m_rail_ov_v  = loadOne("RailMon.OvV", 0.0f, persist);
    m_rail_uv_v  = loadOne("RailMon.UvV", 0.0f, persist);
    if (addr_t >= 8.0f && addr_t <= 119.0f) m_temp_addr = static_cast<uint8_t>(addr_t);
    if (addr_r >= 8.0f && addr_r <= 119.0f) m_rail_addr = static_cast<uint8_t>(addr_r);

    const bool t_ok = probeTemp();
    const bool r_ok = probeRail();
    Telemetry::printf("[I2C] config reloaded: onb_temp=%s @ 0x%02X rail_mon=%s @ 0x%02X",
                      t_ok ? m_temp.partName() : "absent", m_temp_addr,
                      r_ok ? m_rail.partName() : "absent", m_rail_addr);
}

void I2cSensors::update() {
    if (!m_initialized) {
        return;
    }
    const uint32_t now_ms = HAL_GetTick();

    /* Lazy re-probe of absent devices (short timeout, at most once every
     * REPROBE_MS per device — never delays boot or the main loop). */
    if (!tempPresent() && (now_ms - m_temp_reprobe_ms) >= REPROBE_MS) {
        m_temp_reprobe_ms = now_ms;
        if (probeTemp()) {
            Telemetry::printf("[I2C] onb_temp appeared: %s @ 0x%02X",
                              m_temp.partName(), m_temp_addr);
        }
    }
    if (!railPresent() && (now_ms - m_rail_reprobe_ms) >= REPROBE_MS) {
        m_rail_reprobe_ms = now_ms;
        if (probeRail()) {
            Telemetry::printf("[I2C] rail_mon appeared: %s @ 0x%02X",
                              m_rail.partName(), m_rail_addr);
        }
    }

    if ((now_ms - m_last_poll_ms) >= POLL_MS) {
        m_last_poll_ms = now_ms;
        if (tempPresent()) {
            pollTemp(now_ms);
        }
        if (railPresent()) {
            pollRail(now_ms);
        }
    }

    evaluateFaults(now_ms);
}

} // namespace Inverter
