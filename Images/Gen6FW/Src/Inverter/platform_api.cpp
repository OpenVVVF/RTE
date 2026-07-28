#include "platform_api.h"

#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Drivers/Sensors/ApplicationSensors.h"
#include "Inverter/Drivers/Sensors/PhaseCurrentADC.h"
#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Drivers/Sensors/DcLinkCurrentSensor.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
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
    /* Extrapolated to the read instant: smooths the sample staircase between
     * the 5 kHz encoder stream and the 10 kHz FOC steps. */
    return Inverter::encoderADC().extrapolatedAngleDeg();
}

float platform_get_dc_link_voltage(void) {
    return Inverter::dcLinkVoltageSensor().voltage();
}

float platform_get_dc_link_current(void) {
    return Inverter::dcLinkCurrentSensor().current();
}

float platform_get_dc_link_power(void) {
    return Inverter::dcLinkCurrentSensor().power();
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
 * Application sensors — backed by the ApplicationSensors base-image driver.
 *
 * The analog pins are configured in Src/adc.c:
 *   AIN_THROTTLE_A  = PA3 / ADC1_INP15 (shared ADC1/ADC2)
 *   AIN_THROTTLE_B  = PA4 / ADC1_INP18 (shared ADC1/ADC2)
 *   AIN_TMP_SENSE_1 = PA5 / ADC1_INP19
 *   AIN_TMP_SENSE_2 = PA1 / ADC1_INP17
 *   AIN_TMP_SENSE_3 = PA0 / ADC1_INP16
 *   AIN_MOTOR_TMP   = PF4 / ADC3_INP9
 * -------------------------------------------------------------------------- */

float platform_get_throttle_a(void) {
    return Inverter::appSensors().throttleA();
}

float platform_get_throttle_b(void) {
    return Inverter::appSensors().throttleB();
}

bool platform_get_throttle_valid(void) {
    return Inverter::appSensors().throttlePlausible();
}

float platform_get_motor_temperature(void) {
    return Inverter::appSensors().motorTemperatureC();
}

float platform_get_inverter_temperature(uint8_t channel) {
    return Inverter::appSensors().inverterTemperatureC(channel);
}

/* --------------------------------------------------------------------------
 * User digital IO
 *
 * Pins 1..8  -> USER_DIN_1..8  (inputs)
 * Pins 1..4  -> USER_DOUT_1..4 (outputs)
 * Pin  5     -> DEBUG_GREEN_LED, pin 6 -> DEBUG_ORANGE_LED (outputs)
 * -------------------------------------------------------------------------- */

namespace {
struct DioMapEntry {
    GPIO_TypeDef* port;
    uint16_t      pin;
};

constexpr DioMapEntry DIN_MAP[8] = {
    {USER_DIN_1_GPIO_Port, USER_DIN_1_Pin},
    {USER_DIN_2_GPIO_Port, USER_DIN_2_Pin},
    {USER_DIN_3_GPIO_Port, USER_DIN_3_Pin},
    {USER_DIN_4_GPIO_Port, USER_DIN_4_Pin},
    {USER_DIN_5_GPIO_Port, USER_DIN_5_Pin},
    {USER_DIN_6_GPIO_Port, USER_DIN_6_Pin},
    {USER_DIN_7_GPIO_Port, USER_DIN_7_Pin},
    {USER_DIN_8_GPIO_Port, USER_DIN_8_Pin},
};

constexpr DioMapEntry DOUT_MAP[6] = {
    {USER_DOUT_1_GPIO_Port, USER_DOUT_1_Pin},
    {USER_DOUT_2_GPIO_Port, USER_DOUT_2_Pin},
    {USER_DOUT_3_GPIO_Port, USER_DOUT_3_Pin},
    {USER_DOUT_4_GPIO_Port, USER_DOUT_4_Pin},
    {DEBUG_GREEN_LED_GPIO_Port, DEBUG_GREEN_LED_Pin},
    {DEBUG_ORANGE_LED_GPIO_Port, DEBUG_ORANGE_LED_Pin},
};
} // namespace

bool platform_digital_read(uint8_t pin) {
    if (pin < 1 || pin > 8) {
        return false;
    }
    return HAL_GPIO_ReadPin(DIN_MAP[pin - 1].port, DIN_MAP[pin - 1].pin) == GPIO_PIN_SET;
}

void platform_digital_write(uint8_t pin, bool value) {
    if (pin < 1 || pin > 6) {
        return;
    }
    HAL_GPIO_WritePin(DOUT_MAP[pin - 1].port, DOUT_MAP[pin - 1].pin,
                      value ? GPIO_PIN_SET : GPIO_PIN_RESET);
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
