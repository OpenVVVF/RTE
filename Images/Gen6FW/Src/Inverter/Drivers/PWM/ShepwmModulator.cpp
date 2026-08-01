/**
 * @file    ShepwmModulator.cpp
 * @brief   SHEPWM synchronous modulator (Option B: TIM1 forced-output modes).
 *
 * One TIM5 cycle = one electrical cycle (ARR = f_tim / fe).  The switching
 * pattern is a precomputed, ping-pong-buffered event list (timer count,
 * phase, level) expanded from the offline-solved quarter-wave table family
 * in SheTables.h (Tools/she_table_gen.py) with linear interpolation between
 * MI grid points.  A compare-chain ISR on TIM5 CH1 fires one edge ahead and
 * writes TIM1 forced-output modes (OCxM) via the pwm driver; TIM1 always owns
 * the gate pins, so dead time, the BKIN hardware trip and the ADC trigger
 * cadence all stay in circuit.
 *
 * Glitch discipline: the slow context only ever writes the inactive buffer;
 * the single pointer swap (+ARR) happens inside the wrap ISR.  Worst-case
 * failure is "new pattern applies next electrical cycle".
 */

#include "Inverter/Drivers/PWM/Modulator.h"
#include "Inverter/Drivers/PWM/SheTables.h"
#include "Inverter/Drivers/PWM/pwm.h"

#include "main.h"

#include <algorithm>
#include <cmath>

namespace Inverter {
namespace {

/* TIM5 is on APB1: 137.5 MHz timer clock (same as TIM2 in EncoderADC). */
constexpr uint32_t kTim5ClockHz = 137500000UL;

constexpr uint32_t kAnglesPerQuarter = shetab::kAnglesPerQuarter;
/* 4 edges per angle per phase (quarter-wave symmetry) plus the two
 * half-cycle boundary edges (half-wave inversion), 3 phases. */
constexpr uint32_t kMaxEvents = 12U * kAnglesPerQuarter + 6U;

/* Process events due within this many counts of "now" (29 ns): absorbs ISR
 * latency and coincident edges from two phases. */
constexpr uint32_t kDueGuardCounts = 4U;

/* Electrical-angle offset between the FOC theta_e convention (phase-U
 * fundamental peaks at theta_e = +90 deg) and the SHE table convention
 * (phase-U leg switches at theta = 0).  Verified on the scope during
 * handoff bring-up (step 4). */
constexpr float kSheAngleOffsetRad = 1.5707963268f;

constexpr float kTwoPi = 6.283185307f;
constexpr float kPi = 3.1415926536f;

struct SheEvent {
    uint32_t cnt;   /* TIM5 count of the edge within the electrical cycle */
    uint8_t phase;  /* 0=U, 1=V, 2=W */
    uint8_t level;  /* level AFTER the edge: 1 = high-side on, 0 = low-side on */
};

struct ShePattern {
    uint32_t arr = 0;
    uint16_t count = 0;
    SheEvent events[kMaxEvents];
};

ShePattern s_patternBuf[2] __attribute__((section(".dma_buffers")));

class ShepwmModulator final : public Modulator {
public:
    const char* name() const override { return "shepwm"; }
    /* Self-timed on TIM5: neither the TIM1 ISR nor a control caller drives it. */
    bool runsInPwmIsr() const override { return false; }
    void update(float, float, float) override {}
    void commit() override {}

    /**
     * @brief Build the event list for (fe_hz, mi) into the inactive buffer.
     *
     * Slow-context only.  While running, the swap (and the new ARR) applies at
     * the next electrical-cycle wrap; while stopped it just stages the buffer
     * for enter().
     */
    void setPattern(float fe_hz, float mi) {
        if (fe_hz < 0.1f) fe_hz = 0.1f;
        m_fe_hz = fe_hz;
        m_mi = mi;

        const uint8_t idx = (uint8_t)(m_active ^ 1U);
        ShePattern& p = s_patternBuf[idx];
        p.arr = (uint32_t)lroundf((float)kTim5ClockHz / fe_hz) - 1U;
        p.count = buildEvents(p.events, kMaxEvents, interpolateAngles(), p.arr);
        m_pending = idx;        /* staged; applied at next wrap or by enter() */
    }

    /**
     * @brief Phase-locked takeover of the TIM1 outputs.
     *
     * theta_e_rad is the current electrical angle (FOC convention); the timer
     * starts mid-cycle so the pattern continues seamlessly.  Initial pin
     * levels are level-matched to the pattern at the entry angle (no step).
     */
    bool enter(float theta_e_rad, float /*modulation_index*/) override {
        if (m_running) return true;
        if (PWM_IsFocModeActive() || spwmIsRunning()) return false;

        /* Apply a staged pattern immediately (stopped: no wrap needed). */
        if (m_pending != kNoPending) {
            m_active = m_pending;
            m_pending = kNoPending;
        }
        ShePattern& p = s_patternBuf[m_active];
        if (p.count == 0) return false;

        /* SHE-frame angle: table theta=0 is the phase-U positive edge. */
        float theta = theta_e_rad - kSheAngleOffsetRad;
        while (theta < 0.0f) theta += kTwoPi;
        while (theta >= kTwoPi) theta -= kTwoPi;
        const uint32_t start_cnt =
            (uint32_t)((theta / kTwoPi) * (float)(p.arr + 1U)) % (p.arr + 1U);

        /* Level-match every phase to the pattern at the entry angle. */
        for (uint8_t ph = 0; ph < 3U; ++ph) {
            PWM_ForcePhaseLevel(ph, levelOfPhaseAt(p, ph, start_cnt) != 0);
        }

        /* Cursor: first event strictly after start_cnt. */
        m_cursor = 0;
        while (m_cursor < p.count && p.events[m_cursor].cnt <= start_cnt) {
            ++m_cursor;
        }

        __HAL_RCC_TIM5_CLK_ENABLE();
        TIM5->CR1 = 0U;                 /* ARPE=0: ARR writes apply immediately */
        TIM5->PSC = 0U;
        TIM5->ARR = p.arr;
        TIM5->CNT = start_cnt;
        TIM5->CCR1 = (m_cursor < p.count) ? p.events[m_cursor].cnt : p.events[0].cnt;
        TIM5->SR = 0U;                  /* clear any stale flags */
        TIM5->DIER = TIM_DIER_CC1IE | TIM_DIER_UIE;
        HAL_NVIC_SetPriority(TIM5_IRQn, 3, 0);
        HAL_NVIC_EnableIRQ(TIM5_IRQn);
        TIM5->CR1 = TIM_CR1_CEN;

        m_running = true;
        return true;
    }

    void exit() override {
        if (!m_running) return;
        TIM5->CR1 = 0U;
        TIM5->DIER = 0U;
        HAL_NVIC_DisableIRQ(TIM5_IRQn);
        PWM_ReleaseForcedOutputs();
        m_running = false;
    }

    bool isRunning() const { return m_running; }
    float frequencyHz() const { return m_fe_hz; }
    float modulationIndex() const { return m_mi; }
    uint32_t wrapCount() const { return m_wrap_count; }
    uint32_t edgeCount() const { return m_edge_count; }

    /* TIM5 update event = electrical-cycle wrap: the only safe mutation point. */
    void onWrap() {
        ++m_wrap_count;
        m_cursor = 0;
        if (m_pending != kNoPending) {
            m_active = m_pending;
            m_pending = kNoPending;
            TIM5->ARR = s_patternBuf[m_active].arr;   /* ARPE=0: applies immediately */
        }
        processDue();
    }

    /* TIM5 CH1 compare: apply due edges, arm the next one. */
    void onCompare() {
        processDue();
    }

private:
    static constexpr uint8_t kNoPending = 0xFFU;

    /* Interpolate the angle trajectory for m_mi between MI grid points. */
    const float* interpolateAngles() {
        float x = (m_mi - shetab::kMiMin) / shetab::kMiStep;
        if (x < 0.0f) x = 0.0f;
        const float x_max = (float)(shetab::kMiCount - 1U);
        if (x > x_max) x = x_max;
        const uint32_t i = (uint32_t)x;
        const uint32_t j = (i + 1U < shetab::kMiCount) ? i + 1U : i;
        const float f = x - (float)i;
        for (uint32_t k = 0; k < kAnglesPerQuarter; ++k) {
            m_angles[k] = shetab::kAngles[i][k] * (1.0f - f) +
                          shetab::kAngles[j][k] * f;
        }
        return m_angles;
    }

    /**
     * @brief Expand quarter-wave angles into a merged 3-phase event list.
     *
     * Per phase (shift 0 / 120 / 240 deg), the leg waveform starts high at
     * theta=0+, toggles at every angle-derived edge, and inverts at the
     * half-cycle boundary: the boundary crossings at theta=0 and theta=pi are
     * themselves switching instants (2 + 4N edges per phase per cycle), which
     * is what gives the waveform its half-wave symmetry q(t+pi) = -q(t).
     * Events carry the level after the edge.
     */
    static uint16_t buildEvents(SheEvent* out, uint16_t max_events,
                                const float* angles, uint32_t arr) {
        static const float kShift[3] = {
            0.0f, 2.0f * kPi / 3.0f, 4.0f * kPi / 3.0f
        };
        const float scale = (float)(arr + 1U) / kTwoPi;
        uint16_t n = 0;

        auto push = [&](float theta, uint8_t ph, uint8_t level) {
            if (n >= max_events) return;
            if (theta >= kTwoPi) theta -= kTwoPi;
            uint32_t cnt = (uint32_t)lroundf(theta * scale);
            if (cnt > arr) cnt = arr;
            out[n].cnt = cnt;
            out[n].phase = ph;
            out[n].level = level;
            ++n;
        };

        for (uint8_t ph = 0; ph < 3U; ++ph) {
            /* Boundary edge at theta=0 (+shift): leg goes high. */
            push(kShift[ph], ph, 1U);
            uint8_t level = 1U;
            for (uint32_t q = 0; q < 4U; ++q) {
                if (q == 2U) {
                    /* Boundary edge at theta=pi (+shift): half-wave inversion. */
                    push(kShift[ph] + kPi, ph, 0U);
                    level = 0U;
                }
                for (uint32_t i = 0; i < kAnglesPerQuarter; ++i) {
                    const uint32_t idx =
                        (q == 0U || q == 2U) ? i : (kAnglesPerQuarter - 1U - i);
                    float theta;
                    switch (q) {
                    case 0: theta = angles[idx]; break;
                    case 1: theta = kPi - angles[idx]; break;
                    case 2: theta = kPi + angles[idx]; break;
                    default: theta = kTwoPi - angles[idx]; break;
                    }
                    level ^= 1U;
                    push(theta + kShift[ph], ph, level);
                }
            }
        }
        std::sort(out, out + n,
                  [](const SheEvent& a, const SheEvent& b) { return a.cnt < b.cnt; });
        return n;
    }

    /* Level of one phase at a given count: the level after its most recent
     * edge; before its first edge it is the level left by its last edge of
     * the previous cycle (the list wraps). */
    static uint8_t levelOfPhaseAt(const ShePattern& p, uint8_t phase, uint32_t cnt) {
        uint8_t level = 0;
        bool found = false;
        for (uint16_t i = 0; i < p.count; ++i) {
            if (p.events[i].phase != phase) continue;
            if (p.events[i].cnt <= cnt) {
                level = p.events[i].level;
                found = true;
            }
        }
        if (found) return level;
        /* Wrap: take this phase's last edge of the cycle. */
        for (int32_t i = (int32_t)p.count - 1; i >= 0; --i) {
            if (p.events[i].phase == phase) return p.events[i].level;
        }
        return 1;
    }

    /* ISR core: apply every event due by now+guard, arm the next compare. */
    void processDue() {
        const ShePattern& p = s_patternBuf[m_active];
        const uint32_t due = TIM5->CNT + kDueGuardCounts;
        while (m_cursor < p.count && p.events[m_cursor].cnt <= due) {
            const SheEvent& ev = p.events[m_cursor];
            PWM_ForcePhaseLevel(ev.phase, ev.level != 0U);
            ++m_cursor;
            ++m_edge_count;
        }
        if (m_cursor < p.count) {
            TIM5->CCR1 = p.events[m_cursor].cnt;
        } else {
            /* All edges applied; next activity is the wrap handler. */
            TIM5->CCR1 = p.events[0].cnt;
        }
    }

    float m_angles[kAnglesPerQuarter] = {};
    float m_fe_hz = 0.0f;
    float m_mi = 0.0f;
    volatile uint16_t m_cursor = 0;
    volatile uint8_t m_active = 0;
    volatile uint8_t m_pending = kNoPending;
    bool m_running = false;
    uint32_t m_wrap_count = 0;
    uint32_t m_edge_count = 0;
};

ShepwmModulator s_shepwm;

} // namespace

Modulator& shepwmModulator() { return s_shepwm; }

bool shepwmIsRunning() { return s_shepwm.isRunning(); }
void shepwmSetPattern(float fe_hz, float mi) { s_shepwm.setPattern(fe_hz, mi); }
float shepwmFrequencyHz() { return s_shepwm.frequencyHz(); }
float shepwmModulationIndex() { return s_shepwm.modulationIndex(); }
uint32_t shepwmWrapCount() { return s_shepwm.wrapCount(); }
uint32_t shepwmEdgeCount() { return s_shepwm.edgeCount(); }

} // namespace Inverter

/* TIME_DOMAIN: SHEPWM_EDGE_ISR
 *   Rate: 12N+6 events per electrical cycle (N=9: 114/cycle; ~11k/s at 100 Hz,
 *   ~34k/s at 300 Hz).  Hard real-time edge placement; must preempt the
 *   control ISRs (priority 3, above ADC(4) and TIM1_UP(5)) to hold
 *   sub-microsecond edge jitter.  The BKIN hardware trip is a register path,
 *   not an IRQ, and is unaffected.
 * CODEGEN: Keep in the base image; this is a HAL primitive, not model content.
 */
extern "C" void TIM5_IRQHandler(void) {
    const uint32_t sr = TIM5->SR;
    TIM5->SR = ~(sr & (TIM_SR_UIF | TIM_SR_CC1IF));  /* rc_w0: clear served flags */
    if ((sr & TIM_SR_UIF) != 0U) {
        Inverter::s_shepwm.onWrap();
    }
    if ((sr & TIM_SR_CC1IF) != 0U) {
        Inverter::s_shepwm.onCompare();
    }
}
