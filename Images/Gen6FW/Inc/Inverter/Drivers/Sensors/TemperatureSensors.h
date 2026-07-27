#pragma once

#include <cstdint>

namespace Inverter {

/**
 * @brief Slow temperature sensor sampler for the four analog temp inputs.
 *
 * Channels 0..2 are the board NTC inputs AIN_TMP_SENSE_1/2/3
 * (ADC1 INP19/17/16); channel 3 is the motor sensor AIN_MOTOR_TMP
 * (ADC3 INP9).  update() performs one software-triggered conversion per
 * call, round-robin over the channels, and averages 16 raw conversions
 * per channel before computing resistance and temperature.
 *
 * Per-channel conversion parameters and over-temperature thresholds come
 * from the RteParamStore KV namespace (Hw.Temp.B1..3.* for the board
 * channels, Motor.Temp.* for the motor) and are re-read once per second
 * from the RAM cache so `config set` applies live.
 *
 * Faults:
 *  - Open/short sensor sustained 500 ms  -> FaultSource::TempSensor (Warning)
 *  - Board channel over CritC 500 ms     -> FaultSource::OvertemperatureInverter (Critical)
 *  - Motor channel over CritC 500 ms     -> FaultSource::OvertemperatureMotor (Critical)
 */
class TemperatureSensors {
public:
    static constexpr uint8_t NUM_CHANNELS = 4; /**< 0..2 = board, 3 = motor. */
    static constexpr uint8_t BOARD_CHANNELS = 3;

    /** @brief Load config from the KV store and print a boot summary. */
    bool init();

    /**
     * @brief Perform one round-robin conversion; call every main-loop iteration.
     *
     * Also re-reads the KV config cache once per second.
     */
    void update();

    /** @brief Motor temperature [degC], NAN if disabled or out of range. */
    float motorTemperatureC() const;

    /** @brief Board temperature [degC] for channel 0..2, NAN if disabled/out of range. */
    float inverterTemperatureC(uint8_t channel) const;

    /** @brief Per-channel live values for the `temp` shell command. */
    void channelStatus(uint8_t ch, bool& enabled, uint8_t& type, float& volts,
                       float& ohms, float& tempC, bool& outOfRange) const;

    /** @brief Human-readable name for a conversion type (for the shell). */
    static const char* typeName(uint8_t type);

private:
    /* Conversion types (stored as float in the KV store).  Types 1/2/4 use
     * the channel's R25/Beta config; types 3/5/6/7 are named presets with
     * baked constants so a user can flip Hw.Temp.*.Type and compare. */
    static constexpr uint8_t TYPE_DISABLED   = 0;
    static constexpr uint8_t TYPE_NTC_BETA   = 1; /**< custom NTC (R25, Beta)      */
    static constexpr uint8_t TYPE_PTC_BETA   = 2; /**< custom PTC (R25, Beta)      */
    static constexpr uint8_t TYPE_KTY84      = 3; /**< KTY84-130/150 (R25 ~603)    */
    static constexpr uint8_t TYPE_LINEAR_RTD = 4; /**< custom linear (R25, alpha)  */
    static constexpr uint8_t TYPE_PT1000     = 5; /**< PT1000 (1000 ohm @ 0 C)     */
    static constexpr uint8_t TYPE_PT100      = 6; /**< PT100  (100 ohm @ 0 C)      */
    static constexpr uint8_t TYPE_KTY83_110  = 7; /**< KTY83-110 (R25 ~1000, approx)*/

    static constexpr uint8_t  ACCUM_SAMPLES       = 16;
    static constexpr uint32_t FAULT_SUSTAIN_MS    = 500;
    static constexpr float    OOR_OPEN_RATIO      = 0.98f;  /**< of Vcc */
    static constexpr float    OOR_SHORT_RATIO     = 0.02f;  /**< of Vcc */
    static constexpr float    OVERTEMP_HYST_C     = 5.0f;
    static constexpr uint32_t CONFIG_RELOAD_MS    = 1000;

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
        uint32_t raw_accum;
        uint8_t  accum_count;
        bool     out_of_range;
        bool     oor_pending;
        uint32_t oor_since_ms;
        bool     over_temp_cond;
        bool     over_temp_raised;
        uint32_t ot_since_ms;
    };

    void loadConfig(bool persist_defaults);
    bool sampleOnce(uint8_t ch, uint32_t& raw);
    void finishAverage(uint8_t ch, uint32_t now_ms);
    float resistanceToTempC(const Config& cfg, float r) const;
    void updateOutOfRange(uint8_t ch, uint32_t now_ms);
    void updateOverTemp(uint8_t ch, uint32_t now_ms);

    Channel  m_ch[NUM_CHANNELS] = {};
    uint8_t  m_rr = 0;
    float    m_vcc = 3.3f;
    uint32_t m_last_cfg_ms = 0;
    bool     m_initialized = false;
};

/** @brief Global temperature sensor instance. */
TemperatureSensors& temperatureSensors();

} // namespace Inverter
