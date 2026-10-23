/**
  ******************************************************************************
  * @file    InverterMain.cpp
  * @brief   C++ application layer: PWM bring-up + RTE app_loop domain host.
  *
  * Startup behaviour (Phase 1..3 of the Nucleo bring-up plan):
  *   - 10 kHz center-aligned three-phase PWM on TIM1 CH1/CH2/CH3
  *   - complementary CH1N/CH2N/CH3N outputs with 1 us BDTR dead-time
  *   - static duties U=50%, V=25%, W=75% (distinguishable per pin on a scope)
  *   - TIM1 update interrupt running at 10 kHz (hosts the tim_isr domain)
  *
  * RTE-generated code (RTECodeEmitter) replaces the // RTE_EMIT markers below;
  * a tim_isr-domain graph that writes duties overrides the static defaults.
  ******************************************************************************
  */

#include "Inverter/InverterMain.h"
#include "Inverter/AppState.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "platform_api.h"

#include "main.h"
#include "tim.h"

/* Global RTE codegen state variable.  Referenced by app::<DomainTitle>Init/Step
 * calls inserted at // RTE_EMIT markers. */
AppState appState;

static void EnableCycleCounter()
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

extern "C" void InverterMain(void)
{
    EnableCycleCounter();

    /* --- Phase 1/3 PWM bring-up ------------------------------------------ */
    PWM_SetFrequency(PWM_DEFAULT_SWITCHING_FREQ_HZ);   /* 10 kHz  */
    PWM_SetDeadTime(PWM_DEFAULT_DEADTIME_NS);          /* 1 us    */
    PWM_SetComplementary(true);                        /* CHxN active */
    PWM_Start();                                       /* CH1..3 + CH1N..3N */
    PWM_SetThreePhaseDuty(50.0f, 25.0f, 75.0f);        /* U/V/W static duties */

    /* Initialize RTE domain state before the ISR starts stepping it. */
    // RTE_EMIT: tim_isr init

    /* 10 kHz update interrupt: hosts the RTE tim_isr timing domain. */
    PWM_StartUpdateInterrupt();

    // RTE_EMIT: app_loop init

    uint32_t lastBlink = platform_millis();

    for (;;) {
        // RTE_EMIT: app_loop step

        /* Heartbeat: LD2 toggles at 1 Hz so it is obvious the loop is alive. */
        const uint32_t now = platform_millis();
        if (now - lastBlink >= 500U) {
            lastBlink = now;
            HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
        }
    }
}
