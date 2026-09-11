#pragma once

#include "Inverter/Drivers/I2C/I2cBus.h"

#include "i2c.h"

namespace Inverter {

/**
 * @brief II2cBus backed by a HAL I2C handle (firmware builds only).
 *
 * Blocking polling-mode HAL calls with a caller-supplied timeout; no
 * interrupts or DMA.  Errors bump public counters for shell diagnostics.
 */
class HalI2cBus : public II2cBus {
public:
    explicit HalI2cBus(I2C_HandleTypeDef* hi2c) : m_hi2c(hi2c) {}

    bool transfer(uint8_t addr7,
                  const uint8_t* tx, uint8_t tx_len,
                  uint8_t* rx, uint8_t rx_len,
                  uint32_t timeout_ms) override;
    bool isReady(uint8_t addr7, uint32_t timeout_ms) override;

    uint32_t errorCount() const { return m_errors; }
    uint32_t timeoutCount() const { return m_timeouts; }

private:
    I2C_HandleTypeDef* m_hi2c;
    uint32_t m_errors = 0;
    uint32_t m_timeouts = 0;
};

} // namespace Inverter
