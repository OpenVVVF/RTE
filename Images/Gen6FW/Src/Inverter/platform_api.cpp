#include "platform_api.h"

#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Drivers/Sensors/PhaseCurrentADC.h"
#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
#include "Inverter/Drivers/Sensors/TemperatureSensors.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Telemetry.h"

#include "main.h"
#include "adc.h"

/* --------------------------------------------------------------------------
 * PWM / gate-driver outputs
 * -------------------------------------------------------------------------- */

void platform_pwm_set(float du, float dv, float dw) {
    PWM_SetThreePhaseDuty(du, dv, dw);
}

void platform_pwm_set_voltage_vector(float valpha, float vbeta, float vdc) {
    PWM_SetVoltageVector(valpha, vbeta, vdc);
}

/* --------------------------------------------------------------------------
 * Sensor inputs
 * -------------------------------------------------------------------------- */

bool platform_get_phase_currents(float* iu_a, float* iv_a, float* iw_a) {
    if (iu_a == nullptr || iv_a == nullptr || iw_a == nullptr) {
        return false;
    }
    return Inverter::phaseCurrentADC().latest(*iu_a, *iv_a, *iw_a);
}

uint32_t platform_adc_get_injected_u_sig(void) {
    return HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_1);
}

uint32_t platform_adc_get_injected_v_sig(void) {
    return HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_2);
}

uint32_t platform_adc_get_injected_u_ref(void) {
    return HAL_ADCEx_InjectedGetValue(&hadc2, ADC_INJECTED_RANK_1);
}

uint32_t platform_adc_get_injected_v_ref(void) {
    return HAL_ADCEx_InjectedGetValue(&hadc2, ADC_INJECTED_RANK_2);
}

float platform_adc_get_offset_u_a(void) {
    return Inverter::phaseCurrentADC().lastOffsetU();
}

float platform_adc_get_offset_v_a(void) {
    return Inverter::phaseCurrentADC().lastOffsetV();
}

bool platform_get_encoder_angle(float* angle_deg) {
    if (angle_deg == nullptr) {
        return false;
    }
    return Inverter::encoderADC().sample(*angle_deg);
}

float platform_get_encoder_angle_latest(void) {
    return Inverter::encoderADC().lastAngle();
}

float platform_get_dc_link_voltage(void) {
    return Inverter::dcLinkVoltageSensor().voltage();
}

/* Read the driver's 20 kHz DMA snapshot directly - never an ad-hoc SPI
 * transaction from the main loop (racing the burst path corrupts frames and
 * can block for the SPI timeout). */
float platform_phase_voltage_u(void) {
    return Inverter::dcLinkVoltageSensor().adc().voltage(0);
}

float platform_phase_voltage_v(void) {
    return Inverter::dcLinkVoltageSensor().adc().voltage(1);
}

float platform_phase_voltage_w(void) {
    return Inverter::dcLinkVoltageSensor().adc().voltage(2);
}

/* --------------------------------------------------------------------------
 * Application sensors
 * --------------------------------------------------------------------------
 * Temperature channels are sampled by the TemperatureSensors driver
 * (Src/Inverter/Drivers/Sensors/TemperatureSensors.cpp), pumped from the
 * main loop and from platform_sample_application_sensors().
 * Throttle inputs remain codegen placeholders.
 * -------------------------------------------------------------------------- */

static float s_throttle_a = 0.0f;
static float s_throttle_b = 0.0f;

float platform_get_throttle_a(void) {
    return s_throttle_a;
}

float platform_get_throttle_b(void) {
    return s_throttle_b;
}

float platform_get_motor_temperature(void) {
    return Inverter::temperatureSensors().motorTemperatureC();
}

float platform_get_inverter_temperature(uint8_t channel) {
    return Inverter::temperatureSensors().inverterTemperatureC(channel);
}

void platform_sample_application_sensors(void) {
    /* CODEGEN TODO: Add slow ADC sampling here for:
     *   AIN_THROTTLE_A, AIN_THROTTLE_B
     * Update s_throttle_a/b.
     * Temperature channels are handled by the base-image driver. */
    Inverter::temperatureSensors().update();
}

/* --------------------------------------------------------------------------
 * Safety / faults
 * -------------------------------------------------------------------------- */

void platform_raise_fault(uint32_t source, uint8_t reason) {
    Inverter::FaultManager::instance().raise(
        static_cast<Inverter::FaultSource>(source),
        static_cast<Inverter::FaultReason>(reason));
}

bool platform_has_critical_fault(void) {
    return Inverter::FaultManager::instance().isSeverityActive(
        Inverter::FaultSeverity::Critical);
}

/* --------------------------------------------------------------------------
 * Critical sections
 * -------------------------------------------------------------------------- */

void platform_critical_enter(void) {
    __disable_irq();
}

void platform_critical_exit(void) {
    __enable_irq();
}

/* --------------------------------------------------------------------------
 * Config / persistence
 * -------------------------------------------------------------------------- */

float platform_config_load(const char* key, float default_value) {
    if (key == nullptr) return default_value;
    float value = default_value;
    if (Inverter::RteParamStore::isReady()) {
        if (!Inverter::RteParamStore::get(key, &value)) {
            /* Not found: warn, then create with default. */
            Telemetry::printf(
                "[RTE] config key '%s' not in FRAM; using default %.4f",
                key, static_cast<double>(default_value));
            Inverter::RteParamStore::set(key, default_value);
            Inverter::RteParamStore::flush();
            value = default_value;
        }
    }
    return value;
}

void platform_config_set(const char* key, float value) {
    if (key == nullptr) return;
    if (Inverter::RteParamStore::isReady()) {
        Inverter::RteParamStore::set(key, value);
        Inverter::RteParamStore::flush();
    }
}

float platform_config_get(const char* key) {
    if (key == nullptr) return 0.0f;
    float value = 0.0f;
    if (Inverter::RteParamStore::isReady()) {
        Inverter::RteParamStore::get(key, &value);
    }
    return value;
}

/* --------------------------------------------------------------------------
 * Telemetry
 * -------------------------------------------------------------------------- */

void platform_telemetry_log_f32(const char* key, float value) {
    Telemetry::log(key, value);
}

/* --------------------------------------------------------------------------
 * Time
 * -------------------------------------------------------------------------- */

uint32_t platform_millis(void) {
    return HAL_GetTick();
}

uint32_t platform_micros(void) {
    /* DWT cycle counter is enabled by Telemetry::init().  Fall back to ms
     * resolution if it is not running. */
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U) {
        return DWT->CYCCNT / (SystemCoreClock / 1000000U);
    }
    return HAL_GetTick() * 1000U;
}
