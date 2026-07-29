#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief C++ application entry point, called from main() after HAL/clock/
 *        peripheral init.  Never returns; owns the RTE app_loop domain.
 */
void InverterMain(void);

#ifdef __cplusplus
}
#endif
