#include "sil_rt.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cxxabi.h>
#include <deque>
#include <exception>
#include <future>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>

#include "stm32h7xx_hal.h" /* SIL shim: sil_dwt/SystemCoreClock */

extern "C" void InverterMain_Run(void);

namespace {

struct SilRtState {
    std::mutex              mtx;
    std::condition_variable cv;

    uint64_t now_us = 0;             /* simulated time base                */

    /* Time-wait (HAL_Delay) handshake. */
    bool     delay_waiting     = false;
    uint64_t delay_deadline_us = 0;

    /* App-gate ping-pong. */
    uint64_t fw_park_gen       = 0;
    uint64_t sched_release_gen = 0;
    bool     fw_parked         = false;

    /* Deferred host->firmware commands, run FIFO at the next gate entry.
     * A queue (not a single slot) so posts made between gate releases —
     * e.g. a command batch and the engageControl post in the same app
     * period — all execute in order instead of silently overwriting. */
    std::deque<std::function<void()>> pending;

    bool        abort   = false;
    bool        fw_done = false;      /* thread function returned          */
    bool        fw_fail = false;
    std::string fw_error;

    std::thread fw_thread;
};

SilRtState g_rt;

/* Long wall-clock waits use this poll quantum; full stall is reported after
 * kWatchdog wall time so a firmware hang never wedges the harness. */
constexpr auto kWallPoll   = std::chrono::milliseconds(1);
constexpr auto kWatchdog   = std::chrono::seconds(20);

void firmware_thread_main() {
    try {
        InverterMain_Run();
        std::lock_guard<std::mutex> lk(g_rt.mtx);
        g_rt.fw_done = true;
        g_rt.fw_fail = true;
        g_rt.fw_error = "InverterMain_Run returned (firmware main loop exited)";
        g_rt.cv.notify_all();
    } catch (const __cxxabiv1::__forced_unwind&) {
        /* pthread_exit() unwinds as __forced_unwind; swallowing it is what
         * triggered "exception not rethrown".  Always rethrow. */
        throw;
    } catch (const std::exception& e) {
        fprintf(stderr, "[SIL] firmware thread exception: %s\n", e.what());
        std::lock_guard<std::mutex> lk(g_rt.mtx);
        g_rt.fw_done = true;
        g_rt.fw_fail = true;
        g_rt.fw_error = e.what();
        g_rt.cv.notify_all();
    } catch (...) {
        fprintf(stderr, "[SIL] firmware thread: unknown exception\n");
        std::lock_guard<std::mutex> lk(g_rt.mtx);
        g_rt.fw_done = true;
        g_rt.fw_fail = true;
        g_rt.fw_error = "unknown exception on firmware thread";
        g_rt.cv.notify_all();
    }
}

/* Shared schedule-side wait: returns when the firmware blocks at the app
 * gate; pumps while the firmware holds a time-wait.  The pump function
 * drives one fast-tick of simulated work (sil_rt_advance_time_us + sensor
 * and ISR service) and only ever runs while the firmware is blocked. */
bool idle_to_gate_impl(SilPumpFn pump, uint64_t seen_park_gen) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        bool do_pump = false;
        {
            std::unique_lock<std::mutex> lk(g_rt.mtx);
            if (g_rt.fw_done) return false;
            if (g_rt.fw_parked && g_rt.fw_park_gen > seen_park_gen) return true;
            do_pump = g_rt.delay_waiting &&
                      g_rt.now_us < g_rt.delay_deadline_us;
            if (!do_pump) {
                /* Firmware computing (or a just-expired delay not yet
                 * left): poll; the cv predicate re-check happens above. */
                g_rt.cv.wait_for(lk, kWallPoll);
            }
        }
        if (do_pump) {
            pump();
        }
        if (std::chrono::steady_clock::now() - t0 > kWatchdog) {
            std::lock_guard<std::mutex> lk(g_rt.mtx);
            /* Leave the reason in fw_error for the caller's error path; the
             * stderr line alone is lost from main()'s "failed: %s" print. */
            char reason[128];
            std::snprintf(
                reason, sizeof(reason),
                "watchdog: firmware did not reach the app gate "
                "(delay_waiting=%d parked=%d)",
                g_rt.delay_waiting ? 1 : 0, g_rt.fw_parked ? 1 : 0);
            g_rt.fw_error = reason;
            fprintf(stderr, "[SIL] %s\n", g_rt.fw_error.c_str());
            return false;
        }
    }
}

} // namespace

/* --------------------------------------------------------------------------
 * Firmware context
 * ------------------------------------------------------------------------ */

void sil_rt_delay_ms(uint32_t ms) {
    std::unique_lock<std::mutex> lk(g_rt.mtx);
    g_rt.delay_deadline_us = g_rt.now_us + static_cast<uint64_t>(ms) * 1000ULL;
    g_rt.delay_waiting = true;
    g_rt.cv.notify_all();
    while (!g_rt.abort && g_rt.now_us < g_rt.delay_deadline_us) {
        g_rt.cv.wait(lk);
    }
    g_rt.delay_waiting = false;
    if (g_rt.abort) {
        /* Teardown: end the firmware thread cooperatively (SIL-only path;
         * InverterMain_Run has no return channel on real hardware either). */
        lk.unlock();
        pthread_exit(nullptr);
    }
}

void sil_rt_app_gate() {
    std::unique_lock<std::mutex> lk(g_rt.mtx);

    /* Drain the posted host commands on the firmware context, in post order
     * (a command that posts again is run in this same drain pass). */
    while (!g_rt.pending.empty()) {
        auto fn = std::move(g_rt.pending.front());
        g_rt.pending.pop_front();
        lk.unlock();
        fn();                 /* host command on firmware context */
        lk.lock();
    }

    g_rt.fw_parked = true;
    g_rt.fw_park_gen++;
    const uint64_t my_gen = g_rt.fw_park_gen;
    g_rt.cv.notify_all();
    while (!g_rt.abort && g_rt.sched_release_gen != my_gen) {
        g_rt.cv.wait(lk);
    }
    g_rt.fw_parked = false;
    if (g_rt.abort) {
        lk.unlock();
        pthread_exit(nullptr);
    }
}

/* --------------------------------------------------------------------------
 * Scheduler context
 * ------------------------------------------------------------------------ */

void sil_rt_start_firmware() {
    g_rt.fw_thread = std::thread(firmware_thread_main);
}

bool sil_rt_idle_to_gate(SilPumpFn pump) {
    uint64_t seen;
    {
        std::lock_guard<std::mutex> lk(g_rt.mtx);
        seen = g_rt.fw_park_gen;
    }
    return idle_to_gate_impl(pump, seen);
}

bool sil_rt_run_app_iteration(SilPumpFn pump) {
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(g_rt.mtx);
        if (g_rt.fw_done) return false;
        gen = g_rt.fw_park_gen;
        g_rt.sched_release_gen = gen;
        g_rt.cv.notify_all();
    }
    return idle_to_gate_impl(pump, gen);
}

void sil_rt_advance_time_us(uint64_t us) {
    std::lock_guard<std::mutex> lk(g_rt.mtx);
    g_rt.now_us += us;
    /* Fake DWT cycle counter tracks sim time (CYCCNT = us * MHz). */
    sil_dwt.CYCCNT =
        static_cast<uint32_t>(g_rt.now_us * (SystemCoreClock / 1000000ULL));
    if (g_rt.delay_waiting && g_rt.now_us >= g_rt.delay_deadline_us) {
        g_rt.cv.notify_all();
    }
}

void sil_rt_post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_rt.mtx);
    g_rt.pending.push_back(std::move(fn));
    g_rt.cv.notify_all();
}

uint64_t sil_rt_now_us() {
    std::lock_guard<std::mutex> lk(g_rt.mtx);
    return g_rt.now_us;
}

bool sil_rt_fw_delay_waiting() {
    std::lock_guard<std::mutex> lk(g_rt.mtx);
    return g_rt.delay_waiting;
}

bool sil_rt_fw_parked() {
    std::lock_guard<std::mutex> lk(g_rt.mtx);
    return g_rt.fw_parked;
}

void sil_rt_shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_rt.mtx);
        g_rt.abort = true;
        g_rt.cv.notify_all();
    }
    if (!g_rt.fw_thread.joinable()) return;

    /* Timed join: abort is only observed at a delay/gate point, so a
     * firmware spin that never reaches one would wedge join() (and the
     * harness) forever.  Bound the wait, then abandon the thread: the future
     * (holding the thread object, still joinable inside its task) is leaked
     * on purpose — with the firmware still running, unwinding statics is
     * unsafe, so exit the process immediately.  Callers have already
     * flushed the trace/FRAM at this point. */
    auto* join_fut = new std::future<void>(std::async(
        std::launch::async,
        [t = std::move(g_rt.fw_thread)]() mutable { t.join(); }));
    if (join_fut->wait_for(std::chrono::seconds(10)) ==
        std::future_status::timeout) {
        fprintf(stderr, "[SIL] shutdown: firmware thread did not exit within "
                        "10 s of abort (never reached a delay/gate point) — "
                        "detaching and exiting immediately\n");
        fflush(stderr);
        std::quick_exit(0);
    }
    delete join_fut;
}

bool sil_rt_fw_failed() {
    std::lock_guard<std::mutex> lk(g_rt.mtx);
    return g_rt.fw_fail;
}

const char* sil_rt_fw_error() {
    std::lock_guard<std::mutex> lk(g_rt.mtx);
    return g_rt.fw_error.c_str();
}
