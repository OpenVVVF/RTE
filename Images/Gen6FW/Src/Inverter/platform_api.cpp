#include "platform_api.h"

#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Drivers/Sensors/PhaseCurrentADC.h"
#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
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

float platform_get_dc_link_voltage(void) {
    return Inverter::dcLinkVoltageSensor().voltage();
}

/* --------------------------------------------------------------------------
 * Application sensors — placeholders for codegen layer to fill.
 * --------------------------------------------------------------------------
 * The analog pins are already configured in Src/adc.c:
 *   AIN_THROTTLE_A  = PA3 / ADC2_INP15
 *   AIN_THROTTLE_B  = PA4 / ADC2_INP18
 *   AIN_TMP_SENSE_1 = PA5 / ADC1_INP19
 *   AIN_TMP_SENSE_2 = PA1 / ADC1_INP17
 *   AIN_TMP_SENSE_3 = PA0 / ADC1_INP16
 *   AIN_MOTOR_TMP   = PF4 / ADC3_INP9
 *
 * Codegen is expected to add a slow ADC sampler in the app_loop domain and
 * update these variables (or replace these stubs with direct reads).
 * -------------------------------------------------------------------------- */

static float s_throttle_a = 0.0f;
static float s_throttle_b = 0.0f;
static float s_motor_temp = 0.0f;
static float s_inverter_temp[3] = {0.0f, 0.0f, 0.0f};

float platform_get_throttle_a(void) {
    return s_throttle_a;
}

float platform_get_throttle_b(void) {
    return s_throttle_b;
}

float platform_get_motor_temperature(void) {
    return s_motor_temp;
}

float platform_get_inverter_temperature(uint8_t channel) {
    if (channel > 2) {
        return 0.0f;
    }
    return s_inverter_temp[channel];
}

void platform_sample_application_sensors(void) {
    /* CODEGEN TODO: Add slow ADC sampling here for:
     *   AIN_THROTTLE_A, AIN_THROTTLE_B
     *   AIN_TMP_SENSE_1/2/3
     *   AIN_MOTOR_TMP
     * Update s_throttle_a/b, s_motor_temp, s_inverter_temp[].
     */
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
