#include "platform_api.h"

#include "Inverter/Drivers/PWM/pwm.h"
#include "main.h"

#include <string.h>

/* --------------------------------------------------------------------------
 * PWM outputs
 * -------------------------------------------------------------------------- */

void platform_pwm_set(float du, float dv, float dw) {
    PWM_SetThreePhaseDuty(du, dv, dw);
}

void platform_pwm_set_voltage_vector(float valpha, float vbeta, float vdc) {
    PWM_SetVoltageVector(valpha, vbeta, vdc);
}

/* --------------------------------------------------------------------------
 * Sensor inputs - stubs (no sensing hardware wired to the bare Nucleo)
 * -------------------------------------------------------------------------- */

bool platform_get_phase_currents(float* iu_a, float* iv_a, float* iw_a) {
    if (iu_a) *iu_a = 0.0f;
    if (iv_a) *iv_a = 0.0f;
    if (iw_a) *iw_a = 0.0f;
    return false;
}

bool platform_get_encoder_angle(float* angle_deg) {
    if (angle_deg) *angle_deg = 0.0f;
    return false;
}

float platform_get_encoder_angle_latest(void) {
    return 0.0f;
}

float platform_get_dc_link_voltage(void) {
    return 0.0f;
}

float platform_get_throttle_a(void) {
    return 0.0f;
}

float platform_get_throttle_b(void) {
    return 0.0f;
}

/* --------------------------------------------------------------------------
 * Safety / faults - minimal: a critical fault stops all PWM output.
 * -------------------------------------------------------------------------- */

static volatile bool g_critical_fault = false;

void platform_raise_fault(uint32_t source, uint8_t reason) {
    (void)source;
    (void)reason;
    g_critical_fault = true;
    PWM_Stop();
}

bool platform_has_critical_fault(void) {
    return g_critical_fault;
}

/* --------------------------------------------------------------------------
 * Critical sections
 * -------------------------------------------------------------------------- */

static volatile uint32_t g_primask_save = 0;

void platform_critical_enter(void) {
    g_primask_save = __get_PRIMASK();
    __disable_irq();
}

void platform_critical_exit(void) {
    __set_PRIMASK(g_primask_save);
}

/* --------------------------------------------------------------------------
 * Config / persistence - RAM-backed key/value table (lost on reset).
 * -------------------------------------------------------------------------- */

#define CONFIG_MAX_KEYS    16
#define CONFIG_KEY_LEN     32

typedef struct {
    char key[CONFIG_KEY_LEN];
    float value;
    bool used;
} ConfigEntry;

static ConfigEntry g_config[CONFIG_MAX_KEYS];

static ConfigEntry* config_find(const char* key) {
    for (int i = 0; i < CONFIG_MAX_KEYS; ++i) {
        if (g_config[i].used && strncmp(g_config[i].key, key, CONFIG_KEY_LEN) == 0) {
            return &g_config[i];
        }
    }
    return nullptr;
}

float platform_config_load(const char* key, float default_value) {
    if (!key) return default_value;
    ConfigEntry* e = config_find(key);
    if (e) return e->value;

    for (int i = 0; i < CONFIG_MAX_KEYS; ++i) {
        if (!g_config[i].used) {
            strncpy(g_config[i].key, key, CONFIG_KEY_LEN - 1);
            g_config[i].key[CONFIG_KEY_LEN - 1] = '\0';
            g_config[i].value = default_value;
            g_config[i].used = true;
            break;
        }
    }
    return default_value;
}

void platform_config_set(const char* key, float value) {
    if (!key) return;
    ConfigEntry* e = config_find(key);
    if (e) {
        e->value = value;
    } else {
        platform_config_load(key, value);
    }
}

float platform_config_get(const char* key) {
    if (!key) return 0.0f;
    ConfigEntry* e = config_find(key);
    return e ? e->value : 0.0f;
}

/* --------------------------------------------------------------------------
 * Telemetry - no transport on this board yet.
 * -------------------------------------------------------------------------- */

void platform_telemetry_log_f32(const char* key, float value) {
    (void)key;
    (void)value;
}

/* --------------------------------------------------------------------------
 * Time
 * -------------------------------------------------------------------------- */

uint32_t platform_millis(void) {
    return HAL_GetTick();
}

uint32_t platform_micros(void) {
    /* DWT cycle counter, enabled in InverterMain(). 80 cycles per us. */
    return DWT->CYCCNT / (SystemCoreClock / 1000000U);
}
