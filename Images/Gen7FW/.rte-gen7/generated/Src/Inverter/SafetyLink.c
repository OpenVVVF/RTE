#include "Inverter/SafetyLink.h"

#include "main.h"

#include <stdint.h>

/* The startup heartbeat permits the coprocessor to power isolated sensors
 * while the H7 deliberately spends >500 ms in HAL_Delay. After startup, a
 * stalled application loop suppresses edges while actuation is enabled. */
enum { EDGE_PERIOD_MS = 20, ACTIVE_LOOP_TIMEOUT_MS = 500 };

static volatile bool s_enabled;
static volatile bool s_initialized;
static volatile bool s_booting;
static volatile bool s_actuating;
static volatile uint32_t s_last_loop_ms;
static uint32_t s_last_edge_ms;
static bool s_level;

void SafetyLink_Init(void)
{
    s_last_edge_ms = HAL_GetTick();
    s_last_loop_ms = s_last_edge_ms;
    s_level = false;
    s_actuating = false;
    s_booting = true;
    HAL_GPIO_WritePin(COPROCESSOR_SYNC_GPIO_Port, COPROCESSOR_SYNC_Pin,
                      GPIO_PIN_RESET);
    s_initialized = true;
    s_enabled = true;
}

void SafetyLink_Tick(void)
{
    if (!s_enabled) {
        return;
    }
    const uint32_t now_ms = HAL_GetTick();
    if (!s_booting && s_actuating &&
        (uint32_t)(now_ms - s_last_loop_ms) > ACTIVE_LOOP_TIMEOUT_MS) {
        /* A static level is not accepted as a live heartbeat. */
        return;
    }
    if ((uint32_t)(now_ms - s_last_edge_ms) >= EDGE_PERIOD_MS) {
        s_level = !s_level;
        HAL_GPIO_WritePin(COPROCESSOR_SYNC_GPIO_Port, COPROCESSOR_SYNC_Pin,
                          s_level ? GPIO_PIN_SET : GPIO_PIN_RESET);
        s_last_edge_ms = now_ms;
    }
}

void SafetyLink_MainLoop(bool actuating)
{
    s_last_loop_ms = HAL_GetTick();
    s_actuating = actuating;
    s_booting = false;
}

void SafetyLink_Stop(void)
{
    s_enabled = false;
    if (s_initialized) {
        HAL_GPIO_WritePin(COPROCESSOR_SYNC_GPIO_Port, COPROCESSOR_SYNC_Pin,
                          GPIO_PIN_RESET);
    }
}

void SafetyLink_Resume(void)
{
    if (!s_initialized) return;
    s_last_edge_ms = HAL_GetTick();
    s_last_loop_ms = s_last_edge_ms;
    s_actuating = false;
    s_level = false;
    HAL_GPIO_WritePin(COPROCESSOR_SYNC_GPIO_Port, COPROCESSOR_SYNC_Pin,
                      GPIO_PIN_RESET);
    s_enabled = true;
}

void SafetyLink_EmergencyStop(void)
{
    SafetyLink_Stop();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    HAL_GPIO_WritePin(GATE_DRIVER_POWER_ENABLE_GPIO_Port,
                      GATE_DRIVER_POWER_ENABLE_Pin, GPIO_PIN_RESET);
    if (s_initialized) {
        TIM1->EGR = TIM_EGR_BG;
        HAL_GPIO_WritePin(GATE_DRIVER_RESET_GPIO_Port, GATE_DRIVER_RESET_Pin,
                          GPIO_PIN_RESET);
    }
}
