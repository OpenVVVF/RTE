#pragma once

#include <cstdint>
#include <cstddef>

namespace Inverter {

/**
 * @brief PWM-synchronous phase-current ADC for FOC.
 *
 * Uses ADC1+ADC2 in dual-mode injected-simultaneous conversion, triggered by
 * TIM1 TRGO and read from the injected data registers in the ADC ISR.
 *
 * ADC1 injected sequence: U signal (CH4) -> V signal (CH3)
 * ADC2 injected sequence: U reference (CH8) -> V reference (CH7)
 *
 * The two ADCs sample simultaneously rank-by-rank, so U signal/ref and V
 * signal/ref are captured together (true differential measurement).
 * W current is computed as -(iu + iv).
 *
 * ADC2's regular group remains free for other uses (e.g. the encoder DMA).
 */
class PhaseCurrentADC {
public:
    /**
     * @brief Two-point micro-burst sample for one phase.
     *
     * Each phase is sampled twice per injected sequence so the caller can
     * estimate a local slope (di/dt) in addition to the average current.
     */
    struct BurstPoint {
        float iu_a;
        float iv_a;
        uint32_t time_us;
    };

    /**
     * @brief One completed 4-rank injected micro-burst.
     *
     * point[0] and point[1] are consecutive samples of U and V taken at the
     * same trigger.  The local slopes diu/dt and div/dt can be derived from
     * (point[1] - point[0]) / dt.
     */
    struct BurstSample {
        BurstPoint point[2];
        bool valid;
    };

    PhaseCurrentADC() = default;

    /**
     * @brief Initialize hardware: ADC injected channels, TIM1 TRGO, ADC IRQ.
     *
     * Must be called after MX_ADC1_Init(), MX_ADC2_Init(), MX_TIM1_Init()
     * and MX_DMA_Init() have run.
     */
    bool init();

    /**
     * @brief Consume the latest micro-burst sample.
     *
     * Returns false if no new burst is available.  The freshness flag is
     * cleared on success, matching sample() semantics.
     */
    bool sampleBurst(BurstSample& out);

    /**
     * @brief Non-destructive read of the latest micro-burst sample.
     */
    bool latestBurst(BurstSample& out) const;

    /**
     * @brief Timestamp of the last completed burst [us, DWT based].
     */
    uint32_t lastBurstTimeUs() const { return m_last_burst_us; }

    /**
     * @brief Start timer-triggered injected conversions and run a one-shot
     * zero-current offset calibration.  The motor must be at standstill with
     * no phase current.
     */
    bool start();

    /**
     * @brief Stop conversions.
     */
    bool stop();

    /**
     * @brief Re-run the zero-current offset calibration.
     *
     * Only safe when the motor is stopped and no phase current is flowing.
     * Returns false if the ADC is not running.
     */
    bool recalibrateOffsets();

    /**
     * @brief Convert the latest raw samples to amperes.
     *
     * Consume-once semantics for the control loop: the freshness flag is
     * cleared on success, so a second consumer (e.g. telemetry) racing this
     * call will observe stale data.  Use latest() for non-destructive reads.
     *
     * @param[out] iu  Phase U current in A.
     * @param[out] iv  Phase V current in A.
     * @param[out] iw  Phase W current in A (computed).
     * @return true if a new sample pair was available since the last call.
     */
    bool sample(float& iu, float& iv, float& iw);

    /**
     * @brief Read the most recent conversion WITHOUT consuming freshness.
     *
     * Intended for telemetry/logging consumers that run slower than the ADC
     * rate and must not starve the control-loop consumer of sample().
     *
     * @return true if the ADC is running (values valid).
     */
    bool latest(float& iu, float& iv, float& iw) const;

    /**
     * @brief Called from the ADC ISR when an injected sequence completes.
     */
    void onInjectedConversionComplete();

    /**
     * @brief Diagnostic read-back of the latest raw ADC counts.
     */
    uint32_t lastRawUSig() const { return m_raw_u_sig; }
    uint32_t lastRawVSig() const { return m_raw_v_sig; }
    uint32_t lastRawURef() const { return m_raw_u_ref; }
    uint32_t lastRawVRef() const { return m_raw_v_ref; }

    /**
     * @brief Main-loop health check: reference-channel plausibility.
     *
     * A healthy LA37S600 reference is ~1.65 V (KV window
     * Hw.PhCur.RefMinV/MaxV, defaults 1.4/1.9 V).  Checked continuously once
     * armed (the first in-window sighting arms it, so boot-time rail settle
     * can't false-trip); a sustained (500 ms) violation latches
     * FaultSource::CurrentSensorRef — live detection of transducer
     * death/unpowering mid-run.
     */
    void diagnose();
    uint32_t lastRawDclSig() const { return m_raw_dcl_sig; }
    uint32_t lastRawDclRef() const { return m_raw_dcl_ref; }
    float    lastOffsetU() const { return m_offset_u; }
    float    lastOffsetV() const { return m_offset_v; }
    bool     offsetValid() const { return m_offset_valid; }

    /**
     * @brief Latest synchronous current samples (after offset subtraction).
     *
     * These are the same values a future FOC loop would consume: one sample
     * taken at the PWM bottom for each PWM period, not a time-averaged value.
     */
    float    lastU() const { return m_current_u; }
    float    lastV() const { return m_current_v; }

    /**
     * @brief Set the phase-current overcurrent threshold [A].
     *
     * Default is very high (effectively disabled).  The check is applied to
     * the absolute value of U and V currents in the ADC ISR.
     */
    void setOvercurrentThreshold(float amps) { m_oc_threshold_a = amps; }
    float overcurrentThreshold() const { return m_oc_threshold_a; }

    /**
     * @brief Use a captured fixed reference instead of the sampled reference.
     *
     * When enabled, the current reference value is captured and used for all
     * subsequent conversions.  Eliminates reference noise while keeping the
     * current operating point.  Default false.
     */
    void setUseFixedReference(bool use_fixed);
    bool useFixedReference() const { return m_use_fixed_ref; }

    /**
     * @brief Set the hardware ADC analog-watchdog overcurrent threshold [A].
     *
     * A value of 0 disables the watchdog (implemented as a full-range window:
     * AWD1 stays armed so runtime changes only rewrite the threshold
     * registers — stopping conversions to reconfigure corrupts the dual
    /**
     * @brief Set the hardware ADC analog-watchdog overcurrent threshold [A]
     *        (manual fault-injection override, the hwocset command).
     *
     * amps > 0 pins the threshold to that value.  amps == 0 releases the
     * override: the threshold reverts to the derived default (110 % of the
     * Motor.MaxTorqueCurrentA config key when set, otherwise disabled).
     * A value of 0 disables the watchdog (implemented as a full-range window:
     * AWD1 stays armed so runtime changes only rewrite the threshold
     * registers — stopping conversions to reconfigure corrupts the dual
     * injected-simultaneous acquisition).  The window is centered on the
     * sampled zero-current reference codes (not the ideal mid-scale: the
     * reference sits ~480 counts below VREF/2 on this hardware) and watches
     * both injected channels on ADC1.
     * The TIM1/ADC ISR runs permanently for measurement, so reconfiguration
     * is allowed while the drive is idle/stopped/faulted, but the caller must
     * ensure the power stage is not actuating (the hwocset command enforces
     * this).
     * @return true on success, false if the ADC could not be reconfigured.
     */
    bool setHardwareOvercurrentThreshold(float amps);
    float hardwareOvercurrentThreshold() const { return m_hw_oc_threshold_a; }

    /**
     * @brief Arm the AWD from the derived default unless manually overridden.
     *
     * Called periodically (diagnose path) with 110 % of the calibrated max
     * torque current (or 0 when unset).  A no-op while a hwocset override is
     * pinned, and when the derived value is unchanged (avoids register-write
     * churn).
     */
    void setDerivedHardwareOvercurrentThreshold(float amps);

    /**
     * @brief Derived AWD default [A]: 110 % of Motor.MaxTorqueCurrentA, or 0.
     */
    float derivedHwOcThresholdA() const;

    /**
     * @brief Configure the ADC analog watchdog from the stored threshold.
     *
     * Init-time (conversions stopped): full HAL config of mode, thresholds,
     * and interrupt.  Runtime (streams live): threshold-register writes only.
     */
    bool configureAnalogWatchdog();

private:
    bool configureAdcChannels();
    bool initTrigger();
    bool calibrateOffsets();
    float countsToCurrent(uint32_t sig, uint32_t ref) const;
    uint32_t awdHalfWindowCounts() const;
    void awdWindowFromRefs(uint32_t rmin, uint32_t rmax,
                           uint32_t& low, uint32_t& high) const;
    void writeAwdWindow(uint32_t low, uint32_t high);

    static constexpr uint32_t ADC_BITS        = 16;
    static constexpr float    ADC_VREF        = 3.3f;
    static constexpr float    DIVIDER         = 2.0f / 3.0f;
    static constexpr float    SENSITIVITY_VA  = 1.042e-3f; /**< LA37S600. */

    /* Armed-watchdog window tracking: the window is re-framed on the
     * leak-tracked reference extremes (snap to new extremes, relax one count
     * per burst) when an edge moves more than AWD_TRACK_DEADBAND counts.
     * AWD_TRACK_GUARD pads the window beyond the requested half-width.  It is
     * sized from bench measurement: at idle the raw sig codes plunge up to
     * ~230 counts (~17 A equivalent) below the previous burst's reference
     * extremes (isolated-supply/charge-pump noise, single-burst events), so
     * the effective hardware trip level on this bench is roughly
     * requested + 17 A.  Precise overcurrent protection remains the filtered
     * multi-sample software path (setOvercurrentThreshold). */
    static constexpr uint32_t AWD_TRACK_DEADBAND = 8;
    static constexpr uint32_t AWD_TRACK_GUARD    = 240;
    volatile bool    m_awd_armed = false;
    volatile uint32_t m_awd_low  = 0;
    volatile uint32_t m_awd_high = 0;
    volatile uint32_t m_ref_floor = 0;
    volatile uint32_t m_ref_ceil  = 0;

    volatile uint32_t m_raw_u_sig = 0;
    volatile uint32_t m_raw_v_sig = 0;
    volatile uint32_t m_raw_u_ref = 0;
    volatile uint32_t m_raw_v_ref = 0;
    volatile uint32_t m_raw_dcl_sig = 0;
    volatile uint32_t m_raw_dcl_ref = 0;

    /* Micro-burst raw storage: two differential samples per phase per trigger. */
    volatile uint32_t m_raw_burst_u_sig[2] = {0, 0};
    volatile uint32_t m_raw_burst_u_ref[2] = {0, 0};
    volatile uint32_t m_raw_burst_v_sig[2] = {0, 0};
    volatile uint32_t m_raw_burst_v_ref[2] = {0, 0};
    volatile uint32_t m_last_burst_us = 0;

    volatile float    m_iu = 0.0f;
    volatile float    m_iv = 0.0f;
    float             m_offset_u = 0.0f;
    float             m_offset_v = 0.0f;
    bool              m_offset_valid = false;

    /* Latest synchronous current after offset subtraction.  No additional
     * time-domain averaging is applied so this is exactly what a FOC loop
     * running at the PWM sample rate would see. */
    volatile float    m_current_u = 0.0f;
    volatile float    m_current_v = 0.0f;

    float             m_oc_threshold_a = 500.0f;  /**< software OC trip [A] */
    float             m_hw_oc_threshold_a = 0.0f; /**< 0 = ADC watchdog disabled */
    uint8_t           m_oc_count = 0;
    bool              m_use_fixed_ref = false;
    uint32_t          m_fixed_ref_u = 0;
    uint32_t          m_fixed_ref_v = 0;
    static constexpr uint8_t OC_CONSEC_SAMPLES = 3U;

    /* SG-06 over-torque chain: |iq| above 110% of the calibrated max torque
     * current (FRAM key Motor.MaxTorqueCurrentA, 0 = disabled).  Filtered
     * multi-sample software monitor in the ADC ISR; the AWD backstop derives
     * its threshold from the same key unless hwocset pins an override. */
    float             m_over_torque_max_a = 0.0f;
    uint8_t           m_over_torque_count = 0;
    bool              m_hw_oc_manual_override = false;
    static constexpr uint8_t OVER_TORQUE_CONSEC_SAMPLES = 3U;

    volatile bool     m_new_data = false;
    bool              m_running = false;

    /* Reference-plausibility state (diagnose(), main loop). */
    bool              m_ref_armed = false;
    uint32_t          m_ref_implausible_since_ms = 0;
    bool              m_ref_fault_raised = false;
};

/**
 * @brief Global instance used by the ADC ISR.
 */
PhaseCurrentADC& phaseCurrentADC();

} // namespace Inverter
