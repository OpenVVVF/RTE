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
    /* Boot-safe state: the gate-driver rail stays OFF and /RST stays asserted
     * so the power stage can never switch before an explicit start command.
     * Every actuation path (control start, foc start, open-loop start, cal,
     * gatefire) powers the rail and releases /RST itself. */
    HAL_GPIO_WritePin(GATE_DRIVER_POWER_ENABLE_GPIO_Port, GATE_DRIVER_POWER_ENABLE_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GATE_DRIVER_RESET_GPIO_Port, GATE_DRIVER_RESET_Pin, GPIO_PIN_RESET);

    MCP2221A_Printf("[GATE_DRV] Init: held in reset, power rail off (powered on demand)\r\n");
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
