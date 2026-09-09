/*
 * sil_hooks.h — scheduler-side entry points into the SIL sensor/actuator
 * shims.  These are NOT firmware-visible; the scheduler uses them to drive
 * the modeled hardware peripherals.
 *
 * All functions must be called from the scheduler context only (firmware
 * blocked — see sil_rt.h).
 */
#ifndef SIL_HOOKS_H
#define SIL_HOOKS_H

#include <cstddef>
#include <cstdint>

/* --- TIM1 / PWM (sil_pwm.cpp) -------------------------------------------*/
float silTimSwitchingHz();      /* current TRGO (injected trigger) rate     */
float silTimUpdateHz();         /* current update-event rate                */
bool  silTimBaseRunning();      /* HAL_TIM_Base_Start() has run             */
bool  silTimUpdateIrqEnabled(); /* UIE set (PWM_StartUpdateInterrupt)       */
bool  silTimOutputsDriving();   /* MOE + CH1..3(N) running + gate outputs   */

/* Fires HAL_TIM_PeriodElapsedCallback(&htim1) — one update event. */
void  silTimFireUpdateIrq();

/* --- Phase-current ADC (sil_phase_current_adc.cpp) -----------------------*/
bool  silPhaseCurrentAdcRunning();
/* Fires onInjectedConversionComplete() — one injected micro-burst. */
void  silPhaseCurrentAdcTrigger();

/* --- Encoder (sil_encoder_adc.cpp) --------------------------------------*/
bool  silEncoderRunning();
bool  silEncoderSyncTrigger();  /* TIM1-synced vs TIM2 free-running         */
void  silEncoderSampleFromPlant();   /* fill ADC counts, fire DMA hook      */

/* --- Gate driver / GPIO model (sil_hal.cpp) ----------------------------- */
bool  silGateOutputsEnabled();  /* power rail on AND reset released         */

/* --- UART (sil_hal.cpp) --------------------------------------------------*/
/* Fire a deferred UART TX-complete callback if one is pending — call once
 * per app tick while the firmware is blocked. */
void  silUartPumpTxCompletion();

/* Queue client->firmware bytes (live-link RX) into the modeled huart3 FIFO.
 * Call on the scheduler context only (from sil_live_poll).  FIFO is
 * cap-bounded; excess bytes are dropped. */
void  silUartRxEnqueue(const uint8_t* data, size_t len);

/* Deliver queued RX bytes to the armed IT reception: one byte per
 * HAL_UART_RxCpltCallback, exactly like the hardware RXNE interrupt, with
 * the firmware blocked (its callback re-arms for the next byte).  No-op
 * when the FIFO is empty or reception is not armed. */
void  silUartRxPoll();

/* --- Host-side board init (sil_hal.cpp) ----------------------------------*/
/* Peripheral register defaults (TIM1 ARR mirror of MX init, etc.).  Call
 * once before sil_rt_start_firmware(). */
void  sil_hal_init();

#endif /* SIL_HOOKS_H */
