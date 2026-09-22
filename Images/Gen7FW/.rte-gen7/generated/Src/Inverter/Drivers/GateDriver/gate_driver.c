/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gate_driver.c
  * @brief   Gate driver control for NCD57100DWR2G complementary-pair configuration
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "gate_driver.h"
#include "mcp2221a_driver.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* USER CODE BEGIN 1 */

void GateDriver_Init(void)
{
    /* Assert reset (active low) before powering to ensure safe start.  Keep
     * the pulse at 1 ms: the NCx5710y datasheet's 8-10 ms /RST low window
     * invokes the DSCHK diagnostic rather than a plain fault reset. */
    HAL_GPIO_WritePin(GATE_DRIVER_RESET_GPIO_Port, GATE_DRIVER_RESET_Pin, GPIO_PIN_RESET);
    HAL_Delay(1);

    /* Enable gate driver power */
    HAL_GPIO_WritePin(GATE_DRIVER_POWER_ENABLE_GPIO_Port, GATE_DRIVER_POWER_ENABLE_Pin, GPIO_PIN_SET);
    HAL_Delay(50); /* Allow power to stabilize */

    /* Release reset – drivers become operational */
    HAL_GPIO_WritePin(GATE_DRIVER_RESET_GPIO_Port, GATE_DRIVER_RESET_Pin, GPIO_PIN_SET);
    HAL_Delay(10);

    bool ready = GateDriver_IsReady();
    bool fault = GateDriver_IsFault();
    MCP2221A_Printf("[GATE_DRV] Init: READY=%s FAULT=%s\r\n",
                     ready ? "YES" : "NO",
                     fault ? "YES" : "NO");
}

void GateDriver_ResetPulse(void)
{
    /* 1 ms low pulse: clears latched faults while staying under the 8-10 ms
     * DSCHK-invocation window of the NCx5710y. */
    HAL_GPIO_WritePin(GATE_DRIVER_RESET_GPIO_Port, GATE_DRIVER_RESET_Pin, GPIO_PIN_RESET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(GATE_DRIVER_RESET_GPIO_Port, GATE_DRIVER_RESET_Pin, GPIO_PIN_SET);
    HAL_Delay(10);
}

void GateDriver_EnablePower(bool enable)
{
    HAL_GPIO_WritePin(GATE_DRIVER_POWER_ENABLE_GPIO_Port, GATE_DRIVER_POWER_ENABLE_Pin,
                      enable ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void GateDriver_EnableOutputs(void)
{
    /* Release reset (active low). Give the driver time to wake up. */
    HAL_GPIO_WritePin(GATE_DRIVER_RESET_GPIO_Port, GATE_DRIVER_RESET_Pin, GPIO_PIN_SET);
    HAL_Delay(10);
}

void GateDriver_DisableOutputs(void)
{
    /* Assert reset (active low) to force all gate-drive outputs inactive. */
    HAL_GPIO_WritePin(GATE_DRIVER_RESET_GPIO_Port, GATE_DRIVER_RESET_Pin, GPIO_PIN_RESET);
}

bool GateDriver_IsFault(void)
{
    /* /FLT is open-drain active low */
    return (HAL_GPIO_ReadPin(GATE_DRIVER_FAULT_GPIO_Port, GATE_DRIVER_FAULT_Pin) == GPIO_PIN_RESET);
}

bool GateDriver_IsReady(void)
{
    /* /RDY is open-drain active low -> pin HIGH means ready */
    return (HAL_GPIO_ReadPin(GATE_DRIVER_READY_GPIO_Port, GATE_DRIVER_READY_Pin) == GPIO_PIN_SET);
}

/* USER CODE END 1 */
