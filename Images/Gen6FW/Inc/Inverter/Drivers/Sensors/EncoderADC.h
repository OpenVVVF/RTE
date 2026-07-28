#pragma once

#include <cstddef>
#include <cstdint>

namespace Inverter {

/**
 * @brief Analog sin/cos motor encoder read through ADC2 regular conversions.
 *
 * The encoder sin/cos pins (PC0/PC1) are on ADC2 channels 10 and 11.
 * A dedicated DMA stream (DMA2_Stream0) transfers each completed pair to a
 * circular buffer, and a TIM2 TRGO at 10 kHz triggers the regular sequence.
 *
 * Hard limits from the calibrated commit 0eb9f53 are applied first as a
 * fallback.  Learned min/max bounds expand from the observed signal as the
 * encoder rotates; once both channels have spanned LEARNED_MIN_SPAN counts
 * they replace the hardcoded fallback for normalization, so the angle
 * self-calibrates after roughly one revolution.  The angle is computed with
 * atan2 and returned in degrees [0, 360).
 */
class EncoderADC {
public:
    EncoderADC() = default;

    /**
     * @brief Initialize ADC2 regular channels, TIM2 trigger, and DMA.
     *
     * Must be called after MX_ADC2_Init() and MX_DMA_Init() have run.
     */
    bool init();

    /**
     * @brief Start regular DMA conversions and the TIM2 trigger.
     */
    bool start();

    /**
     * @brief Read the latest encoder angle.
     *
     * @param[out] angle_deg  Encoder angle in degrees, 0..360.
     * @return true if a new sample was available since the last call.
     */
    bool sample(float& angle_deg);

    /**
     * @brief Atomically read the latest encoder angle and raw sin/cos values.
     *
     * Guarantees that the three returned values came from the same ADC DMA
     * completion, which is necessary for diagnostics and calibration.
     *
     * @param[out] angle_deg  Encoder angle in degrees, 0..360.
     * @param[out] raw_sin    Raw ADC count for the sin channel.
     * @param[out] raw_cos    Raw ADC count for the cos channel.
     * @return true if a new sample was available since the last call.
     */
    bool sample(float& angle_deg, uint16_t& raw_sin, uint16_t& raw_cos);

    /**
     * @brief Latest raw ADC counts and computed angle.
     */
    uint32_t lastRawSin() const { return m_snapshot.raw_sin; }

    /** Install previously learned envelope bounds (e.g. restored from FRAM at
     * boot) so the decoder does not fall back to full-scale caps. */
    void setLearnedBounds(uint16_t sinMin, uint16_t sinMax,
                          uint16_t cosMin, uint16_t cosMax) {
        m_obs_sin_min = sinMin;
        m_obs_sin_max = sinMax;
        m_obs_cos_min = cosMin;
        m_obs_cos_max = cosMax;
    }
    uint32_t lastRawCos() const { return m_snapshot.raw_cos; }
    float    lastAngle() const { return m_snapshot.angle; }

    /**
     * @brief HAL tick of the most recent DMA sample completion.
     *
     * Lets control loops verify the encoder stream is alive without depending
     * on main-loop scheduling (unlike diagnose(), which can false-trip when
     * the main loop is busy).
     */
    uint32_t lastSampleMs() const { return m_last_sample_ms; }

    /**
     * @brief Current amplitude bounds used for normalization.
     *
     * Returns the learned bounds once they are valid, otherwise the
     * fallback (hardcoded caps or a manual setBounds() override).
     */
    uint16_t sinMin() const { return m_active_sin_min; }
    uint16_t sinMax() const { return m_active_sin_max; }
    uint16_t cosMin() const { return m_active_cos_min; }
    uint16_t cosMax() const { return m_active_cos_max; }

    /**
     * @brief True once the learned bounds have seen enough span on both
     * channels to be used for normalization (roughly one revolution).
     */
    bool learnedBoundsActive() const { return m_learned_active; }

    /**
     * @brief True once both sin and cos have seen enough variation to compute
     * a meaningful angle.
     */
    bool boundsValid() const {
        return (m_sin_max > m_sin_min) && (m_cos_max > m_cos_min);
    }

    /**
     * @brief Mechanical speed in RPM, signed by direction.
     *
     * Derived in the DMA ISR from unwrapped angle deltas at the 10 kHz
     * sample rate, low-passed with an EMA (~100 ms settling).
     */
    float rpmMech() const { return m_rpm_ema; }

    /**
     * @brief DMA completion callback, called from DMA2_Stream0_IRQHandler.
     */
    void onDmaComplete();

    /**
     * @brief Dump the angle-linearity trace via Telemetry.
     *
     * A 1 kHz-decimated ring (1024 samples ~ 1 s) of raw sin/cos + decoded
     * angle, captured in the 10 kHz DMA ISR.  Used with the motor spinning
     * to measure per-revolution decode nonlinearity without telemetry
     * timestamp jitter (`enc_trace` shell command).
     */
    void traceDump();

    /**
     * @brief Latest angle extrapolated to NOW [deg, 0..360).
     *
     * snapshot angle + rpm_ema * age (DWT cycle counter, microsecond
     * precision).  The encoder samples at the TIM1 update rate (5 kHz) while
     * the FOC steps at 10 kHz; without extrapolation the commutation angle
     * staircases by (speed x sample period) every other step.  The
     * correction is bounded to one sample period's worth of rotation, so a
     * stalled stream degrades to the raw snapshot, never a runaway angle.
     */
    float extrapolatedAngleDeg();

    /**
     * @brief Select the encoder sample trigger source.
     *
     * false = TIM2 free-running 10 kHz (idle/calibration: always sampling).
     * true  = TIM1 TRGO2 update event (control running: angle stream is
     * synchronous with the FOC timebase — no independent-clock beat, no
     * stall/catch-up angle steps at speed).  The DMA stream keeps running;
     * only the trigger select bits change.  The RPM estimator's time base
     * self-calibrates from the measured sample rate either way.
     */
    void useSynchronizedTrigger(bool sync);

    /**
     * @brief DMA error callback for the encoder ADC stream.
     */
    void onDmaError();

    /**
     * @brief Main-loop health check, RPM estimate, and fault evaluation.
     *
     * The DMA ISR only decodes and publishes the angle snapshot; everything
     * else (mechanical-speed estimate, amplitude-collapse/rail faults,
     * sample-rate telemetry, stalled-stream check) runs here at main-loop
     * cadence.  Call once per main-loop iteration.
     */
    void diagnose();

    /**
     * @brief Set fixed sin/cos amplitude bounds.  Seeds the learned bounds so
     * the override takes effect immediately; they keep expanding from real
     * samples afterwards.  The hard caps are not changed.
     */
    void setBounds(uint16_t sin_min, uint16_t sin_max,
                   uint16_t cos_min, uint16_t cos_max);

    /**
     * @brief Reset dynamic bounds to the hard-coded caps and clear the
     * learned bounds.
     */
    void resetBounds();

    /**
     * @brief No-op kept for API compatibility with existing calibrators.
     *
     * Dynamic bounds are always updated inside computeAngle().
     */
    void learnBounds(bool enable) { (void)enable; }

private:
    bool configureAdcChannels();
    bool initTimer();
    bool initDma();
    float computeAngle(uint16_t raw_sin, uint16_t raw_cos);

    /* Hard limits measured/calibrated in commit 0eb9f53.  Used as the
     * fallback normalization bounds until the learned bounds become valid. */
    static constexpr uint16_t SIN_MIN_CAP = 427U;
    static constexpr uint16_t SIN_MAX_CAP = 65388U;
    static constexpr uint16_t COS_MIN_CAP = 608U;
    static constexpr uint16_t COS_MAX_CAP = 64743U;

    /* Minimum span [counts] per channel before learned bounds are trusted. */
    static constexpr uint16_t LEARNED_MIN_SPAN = 15000U;

    /* Fallback bounds: the hard caps by default, or a manual override via
     * setBounds().  Never mutated by the sampling ISR. */
    uint16_t m_sin_min = SIN_MIN_CAP;
    uint16_t m_sin_max = SIN_MAX_CAP;
    uint16_t m_cos_min = COS_MIN_CAP;
    uint16_t m_cos_max = COS_MAX_CAP;

    /* Learned bounds: expand from the observed signal as the encoder rotates.
     * Start empty so they track the true signal range of the attached
     * hardware instead of trusting stale hardcoded values. */
    uint16_t m_obs_sin_min = 65535U;
    uint16_t m_obs_sin_max = 0U;
    uint16_t m_obs_cos_min = 65535U;
    uint16_t m_obs_cos_max = 0U;

    /* Bounds actually used for normalization (learned once valid, else
     * fallback).  Written by the ISR, read by the main-loop accessors. */
    volatile uint16_t m_active_sin_min = SIN_MIN_CAP;
    volatile uint16_t m_active_sin_max = SIN_MAX_CAP;
    volatile uint16_t m_active_cos_min = COS_MIN_CAP;
    volatile uint16_t m_active_cos_max = COS_MAX_CAP;
    volatile bool     m_learned_active = false;

    /**
     * @brief Atomic snapshot of one encoder sample.
     *
     * The ISR writes all three fields and then sets m_new_data.  The main loop
     * copies the whole snapshot with interrupts disabled, guaranteeing that
     * angle, raw_sin, and raw_cos are all from the same ADC DMA completion.
     */
    struct Snapshot {
        float    angle = 0.0f;
        uint16_t raw_sin = 0U;
        uint16_t raw_cos = 0U;
    };

    volatile Snapshot m_snapshot;
    volatile bool     m_new_data = false;
    bool              m_running = false;
    volatile uint32_t m_last_sample_cycles = 0;  /**< DWT->CYCCNT at DMA completion */

    /* Angle-linearity trace ring: every 10th DMA sample (~1 kHz), ~1 s of
     * raw sin/cos + decoded angle for offline per-rev analysis. */
    struct TraceEntry {
        uint16_t raw_sin;
        uint16_t raw_cos;
        float    angle_deg;
    };
    static constexpr size_t   TRACE_LEN = 1024;
    static constexpr uint8_t  TRACE_DECIM = 10;
    TraceEntry m_trace[TRACE_LEN] = {};
    size_t     m_trace_head = 0;
    uint8_t    m_trace_decim = 0;

    /* Fault-detection state (evaluated in diagnose() at main-loop cadence). */
    static constexpr uint32_t SAMPLE_TIMEOUT_MS = 5U;
    static constexpr uint16_t MIN_AMP_RANGE     = 20000U;
    static constexpr float    AMP_COLLAPSE_THRESHOLD = 500.0f;
    static constexpr float    MAG_EMA_ALPHA     = 0.2f;   /**< per diagnose() call */
    static constexpr uint16_t AMP_COLLAPSE_COUNT  = 25U;  /**< consecutive calls */
    static constexpr uint16_t RAIL_MARGIN         = 200U;
    static constexpr uint16_t RAIL_COUNT          = 10U;  /**< consecutive calls */

    volatile uint32_t m_last_sample_ms = 0;
    volatile uint16_t m_amp_low_count  = 0;
    volatile uint16_t m_rail_count     = 0;
    float             m_mag_ema        = 0.0f;
    bool              m_mag_ema_init   = false;
    volatile uint32_t m_isr_count      = 0;

    /* Mechanical speed estimation (main loop, time-based window).
     * m_sample_hz is measured from the actual trigger rate in diagnose(). */
    static constexpr float    RPM_ALPHA     = 0.005f;
    static constexpr uint32_t RPM_WINDOW_MS = 40U;  /* long enough to average
        away per-sample angle noise (EMI) that a 1-sample delta amplifies. */
    float m_sample_hz    = 10000.0f;
    float m_rpm_prev_angle = 0.0f;
    float m_rpm_filt_angle = 0.0f;
    float m_unwrapped_angle = 0.0f;
    float m_window_ref_angle = 0.0f;
    uint32_t m_rpm_window_ms = 0;
    bool  m_rpm_init       = false;
    volatile float m_rpm_ema = 0.0f;
};

/**
 * @brief Global instance used by the DMA ISR.
 */
EncoderADC& encoderADC();

} // namespace Inverter
