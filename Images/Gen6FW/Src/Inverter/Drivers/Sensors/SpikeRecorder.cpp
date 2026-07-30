#include "Inverter/Drivers/Sensors/SpikeRecorder.h"

#include "Inverter/Telemetry.h"

#include <cmath>

namespace Inverter {

namespace {
SpikeRecorder s_instance;
}

SpikeRecorder& spikeRecorder() {
    return s_instance;
}

void SpikeRecorder::setThreshold(float amps) {
    if (amps > 0.0f) {
        m_threshold = amps;
    }
}

void SpikeRecorder::onSample(uint32_t tick_ms,
                             uint16_t raw_u_sig, uint16_t raw_v_sig,
                             uint16_t raw_u_ref, uint16_t raw_v_ref,
                             float iu, float iv,
                             float enc_angle, uint16_t enc_sin, uint16_t enc_cos,
                             float duty_u, float duty_v, float duty_w) {
    if (m_ready) {
        return;  // frozen, waiting for dump — do NOT overwrite the capture
    }

    Sample& s = m_ring[m_head];
    s.tick_ms   = tick_ms;
    s.raw_u_sig = raw_u_sig;
    s.raw_v_sig = raw_v_sig;
    s.raw_u_ref = raw_u_ref;
    s.raw_v_ref = raw_v_ref;
    s.enc_sin   = enc_sin;
    s.enc_cos   = enc_cos;
    s.iu        = iu;
    s.iv        = iv;
    s.enc_angle = enc_angle;
    s.duty_u    = duty_u;
    s.duty_v    = duty_v;
    s.duty_w    = duty_w;

    const size_t this_idx = m_head;
    m_head = (m_head + 1) % RING;

    if (m_post_remaining >= 0) {
        if (--m_post_remaining == 0) {
            m_ready = true;
        }
        return;
    }

    if (tick_ms < m_holdoff_until_ms) {
        return;
    }

    if (std::fabs(iu) > m_threshold || std::fabs(iv) > m_threshold) {
        m_trigger_idx = this_idx;
        m_post_remaining = POST_TRIGGER;
        ++m_trigger_count;
        m_holdoff_until_ms = tick_ms + 50;  // retrigger holdoff
    }
}

void SpikeRecorder::dump() {
    if (!m_ready) {
        Telemetry::printf("[SHELL] spikes: no capture (triggers so far: %lu, threshold %.1f A)",
                          static_cast<unsigned long>(m_trigger_count),
                          static_cast<double>(m_threshold));
        return;
    }

    Telemetry::printf("[SHELL] spikes: capture #%lu (* = trigger), %d samples @ 5 kHz",
                      static_cast<unsigned long>(m_trigger_count),
                      static_cast<int>(RING));

    float prev_angle = NAN;
    for (size_t k = 0; k < RING; ++k) {
        /* Oldest first: the newest sample is POST_TRIGGER slots after the
         * trigger, so the oldest is one slot past that. */
        const size_t idx = (m_trigger_idx + POST_TRIGGER + 1 + k) % RING;
        const Sample& s = m_ring[idx];

        float dang = NAN;
        if (std::isfinite(prev_angle)) {
            dang = s.enc_angle - prev_angle;
            if (dang > 180.0f) dang -= 360.0f;
            else if (dang < -180.0f) dang += 360.0f;
        }
        prev_angle = s.enc_angle;

        const bool is_trigger = (idx == m_trigger_idx);
        Telemetry::printf("[SHELL] spk%c%02d t=%lu iu=%7.1f iv=%7.1f ang=%6.1f dang=%+5.2f "
                          "du=%5.1f dv=%5.1f dw=%5.1f sin=%5u cos=%5u",
                          is_trigger ? '*' : ' ',
                          static_cast<int>(k),
                          static_cast<unsigned long>(s.tick_ms),
                          static_cast<double>(s.iu),
                          static_cast<double>(s.iv),
                          static_cast<double>(s.enc_angle),
                          static_cast<double>(dang),
                          static_cast<double>(s.duty_u),
                          static_cast<double>(s.duty_v),
                          static_cast<double>(s.duty_w),
                          s.enc_sin, s.enc_cos);
    }

    m_ready = false;
    m_post_remaining = -1;
}

} // namespace Inverter
