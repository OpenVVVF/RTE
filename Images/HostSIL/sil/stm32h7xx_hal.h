/*
 * Forwarder for firmware headers that do #include "../stm32h7xx_hal.h"
 * (e.g. Inc/Inverter/Telemetry.h).  With sil/stm32shim on the include path,
 * the quoted relative include resolves to <stm32shim>/../stm32h7xx_hal.h,
 * i.e. this file, once the per-file-directory lookup misses.
 */
#ifndef SIL_STM32H7XX_HAL_FORWARDER_H
#define SIL_STM32H7XX_HAL_FORWARDER_H

#include "stm32shim/stm32h7xx_hal.h"

#endif
