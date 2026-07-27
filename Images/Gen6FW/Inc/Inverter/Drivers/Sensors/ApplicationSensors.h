#pragma once

#include <cstdint>

namespace Inverter {

/**
 * @brief Slow application analog sampler: temperatures + throttle inputs.
 *
 * All hardware capture runs without CPU involvement:
 *  - TIM3 TRGO at 1 kHz triggers an ADC1 regular scan of 5 ranks
 *    (board temps 1..3 + throttle A/B) into a circular DMA buffer.
 *  - ADC3 (motor temp, INP9) free-runs in continuous mode; update() harvests
 *    completed conversions without ever blocking.
 *
 * The CPU never waits on an ADC: no HAL_ADC_PollForConversion, no runtime
 * HAL_ADC_Start/Stop, no runtime ADC reconfiguration.  update() only
 * harvests finished conversions and does arithmetic.
 *
 * Per-channel conversion parameters come from the RteParamStore KV
 * namespaces Hw.Temp.B1..3.* (board) and Motor.Temp.* (motor), loaded once
 * at init; reload via the `temp reload` shell command.
 */
class ApplicationSensors {
public:
    static constexpr uint8_t NUM_CHANNELS = 4; /**< 0..2 = board, 3 = motor. */
    static constexpr uint8_t BOARD_CHANNELS = 3;
    static constexpr uint8_t ADC1_RANKS = 5;   /**< 3 temps + 2 throttle. */

    /** @brief Load config from the KV store, start ADC3 (and later TIM3+DMA). */
    bool init();

    /**
     * @brief Harvest ADC3 conversions and recompute channel values; call every
     * main-loop iteration.  Never blocks on the ADC.
     */
    void update();

    /** @brief Reload KV conversion config (for the `temp reload` shell command). */
    void reloadConfig();

    /** @brief Motor temperature [degC], NAN if disabled or out of range. */
    float motorTemperatureC() const;

    /** @brief Board temperature [degC] for channel 0..2, NAN if disabled/out of range. */
    float inverterTemperatureC(uint8_t channel) const;

    /** @brief Throttle input pin voltages [V] (raw). */
    float throttleAVoltage() const;
    float throttleBVoltage() const;

    /** @brief Throttle channels normalized [0..1] via KV min/max; 0 while implausible. */
    float throttleA() const;
    float throttleB() const;

    /** @brief true while throttle A/B agree within the plausibility tolerance. */
    bool throttlePlausible() const;

    /** @brief Per-channel live values for the `temp` shell command. */
    void channelStatus(uint8_t ch, bool& enabled, uint8_t& type, float& volts,
                       float& ohms, float& tempC, bool& outOfRange) const;

    /** @brief Debug dump of ADC state for the `temp` shell command. */
    void debugStatus() const;

    /** @brief Human-readable name for a conversion type (for the shell). */
    static const char* typeName(uint8_t type);

private:
    /* Conversion types (stored as float in the KV store).  Types 1/2/4 use
     * the channel's R25/Beta config; types 3/5/6/7 are named presets with
     * baked constants so a user can flip *.Type and compare. */
    static constexpr uint8_t TYPE_DISABLED   = 0;
    static constexpr uint8_t TYPE_NTC_BETA   = 1; /**< custom NTC (R25, Beta)      */
    static constexpr uint8_t TYPE_PTC_BETA   = 2; /**< custom PTC (R25, Beta)      */
    static constexpr uint8_t TYPE_KTY84      = 3; /**< KTY84-130/150 (R25 ~603)    */
    static constexpr uint8_t TYPE_LINEAR_RTD = 4; /**< custom linear (R25, alpha)  */
    static constexpr uint8_t TYPE_PT1000     = 5; /**< PT1000 (1000 ohm @ 0 C)     */
    static constexpr uint8_t TYPE_PT100      = 6; /**< PT100  (100 ohm @ 0 C)      */
    static constexpr uint8_t TYPE_KTY83_110  = 7; /**< KTY83-110 (R25 ~1000, approx)*/

    static constexpr uint32_t WINDOW_MS           = 16; /**< recompute cadence ~60 Hz. */
    static constexpr float    OOR_OPEN_RATIO      = 0.98f;  /**< of Vcc */
    static constexpr float    OOR_SHORT_RATIO     = 0.02f;  /**< of Vcc */
    static constexpr float    THROTTLE_PLAUS_TOL  = 0.10f;  /**< |A-B| normalized */
    static constexpr uint32_t THROTTLE_PLAUS_MS   = 100;    /**< sustain before fault */

    struct Config {
        bool    enabled;
        uint8_t type;
        float   r25;
        float   beta;
        float   rser;
        uint8_t orient;   /**< 0 = sensor to GND (RSer pull-up), 1 = sensor to VCC. */
        float   crit_c;
    };

    struct Channel {
        Config   cfg;
        float    temp_c;
        float    voltage;
        float    resistance;  /**< Last computed divider resistance [ohm], NAN if invalid. */
        bool     out_of_range;
    };

    void computeWindow();
    void evaluateChannel(uint8_t ch);
    void updateThrottlePlausibility(uint32_t now_ms);
    bool configureAdc1ScanDma();
    void loadConfig(bool persist_defaults);
    float resistanceToTempC(const Config& cfg, float r) const;

    Channel  m_ch[NUM_CHANNELS] = {};
    float    m_thr_a_v = 0.0f;
    float    m_thr_b_v = 0.0f;
    float    m_thr_a_norm = 0.0f;
    float    m_thr_b_norm = 0.0f;
    float    m_thr_a_cand = 0.0f;  /**< normalized, pre-plausibility */
    float    m_thr_b_cand = 0.0f;  /**< normalized, pre-plausibility */
    float    m_thr_min_v[2] = {0.5f, 0.5f};
    float    m_thr_max_v[2] = {4.5f, 4.5f};
    bool     m_thr_plausible = true;
    bool     m_thr_fault_raised = false;
    uint32_t m_thr_implausible_since = 0;
    uint32_t m_adc3_latest = 0;
    float    m_vcc = 3.3f;
    uint32_t m_last_window_ms = 0;
    bool     m_initialized = false;
};

/** @brief Global application sensors instance. */
ApplicationSensors& appSensors();

} // namespace Inverter
