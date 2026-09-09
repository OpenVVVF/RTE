#pragma once

/**
 * @brief Runtime parameter descriptor for RTE-generated code (HostSim stub).
 *
 * Generated domain files may emit a table of these so the base image can
 * get/set node parameters by name at runtime. HostSim does not implement a
 * parameter shell yet; the struct must exist for codegen compatibility.
 */
struct RteParamDesc {
    const char* name;
    void (*set)(void* state, float value);
    float (*get)(const void* state);
};

/* ----------------------------------------------------------------------------
 * HostSim phase-current ADC model constants.
 *
 * These mirror the Gen6FW PhaseCurrentADC signal chain (STM32H7 16-bit ADC,
 * resistor divider, LA37S600 current transducer with a 1.65 V zero-current
 * reference rail).  platform_api.cpp uses them as the ideal defaults; scenario
 * JSON "adc" keys can deviate from them to inject gain/offset/noise errors.
 * -------------------------------------------------------------------------- */
#if defined(__cplusplus)
namespace hostsim { namespace adc {

constexpr unsigned kBits             = 16u;            /* ADC resolution        */
constexpr float    kVrefV            = 3.3f;           /* ADC reference         */
constexpr float    kRefVolts         = 1.65f;          /* zero-current rail      */
constexpr float    kDivider          = 2.0f / 3.0f;    /* input resistor divider */
constexpr float    kSensitivityVPerA = 1.042e-3f;      /* LA37S600 transducer    */
constexpr unsigned kMaxCounts        = (1u << kBits) - 1u;

}} // namespace hostsim::adc
#endif
