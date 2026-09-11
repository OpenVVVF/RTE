/*
 * sil_rt.h — cooperative two-context runtime for HostSIL.
 *
 * The firmware (InverterMain_Run) runs on a dedicated host thread (the
 * "firmware context"); the SIL scheduler runs on the main thread.  The two
 * contexts are strictly exclusive: the firmware blocks in exactly two places
 *
 *   HAL_Delay(ms)          -> sil_rt_delay_ms()   (time-wait)
 *   EncoderADC::diagnose() -> sil_rt_app_gate()   (once per loop() pass)
 *
 * and the scheduler only advances the simulated clock / fires sensor and ISR
 * hooks while the firmware context is blocked.  No per-variable locking of
 * firmware state is needed anywhere ("cooperative hardware": on real hardware
 * ISRs preempt at any instruction; here they run at tick boundaries — see
 * README for the fidelity note).
 *
 * Boot sequence: sil_rt_start_firmware(), then sil_rt_idle_to_gate(pump)
 * waits until firmware init() has run and loop() first parks at the app gate;
 * while the firmware sits in a boot-time HAL_Delay the supplied pump callback
 * is invoked (advancing sim time and feeding sensor/plant hooks).
 */
#ifndef SIL_RT_H
#define SIL_RT_H

#include <cstdint>
#include <functional>

/* Firmware-context entry points ----------------------------------------*/

/* Block the firmware context for `ms` milliseconds of simulated time. */
void sil_rt_delay_ms(uint32_t ms);

/* App-loop rendezvous.  Runs any posted host command on the firmware
 * context, then parks until the scheduler releases the next iteration. */
void sil_rt_app_gate();

/* Scheduler-context entry points ---------------------------------------*/

using SilPumpFn = const std::function<void()>&;

/* Launch InverterMain_Run() on the firmware thread. */
void sil_rt_start_firmware();

/* Wait until the firmware context is parked at the app gate (next park).
 * While the firmware is blocked in HAL_Delay, keep calling pump() to
 * advance sim time; while it is merely computing, sleep-poll briefly.
 * Returns false if the firmware thread died or the wall-clock watchdog
 * fired (deadlock diagnostic). */
bool sil_rt_idle_to_gate(SilPumpFn pump);

/* Release the parked firmware for one app_loop iteration and wait until it
 * re-parks (pumping through any HAL_Delay it performs). */
bool sil_rt_run_app_iteration(SilPumpFn pump);

/* Advance simulated time by `us` (scheduler context only; firmware must be
 * blocked).  Wakes delay-waiters via the condition predicate. */
void sil_rt_advance_time_us(uint64_t us);

/* Queue `fn` to run on the firmware context at the next app-gate entry,
 * before the firmware parks.  Posts are queued FIFO: any number posted
 * between gate releases all execute, in post order. */
void sil_rt_post(std::function<void()> fn);

/* Current simulated time in microseconds. */
uint64_t sil_rt_now_us();

/* True while the firmware is blocked in HAL_Delay (scheduler safe window). */
bool sil_rt_fw_delay_waiting();

/* True while the firmware is parked at the app gate. */
bool sil_rt_fw_parked();

/* Request teardown and join the firmware thread. */
void sil_rt_shutdown();

bool sil_rt_fw_failed();
const char* sil_rt_fw_error();

#endif /* SIL_RT_H */
