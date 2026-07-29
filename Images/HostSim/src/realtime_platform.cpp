#include "realtime_platform.h"

#include <cstdio>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")
#endif

namespace hostsim {

RealtimeSession::RealtimeSession() {
#ifdef _WIN32
    if (timeBeginPeriod(1) == TIMERR_NOERROR) {
        timer_period_ms_ = 1;
        active_ = true;
    }

    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
        active_ = true;
    }

    DWORD mmcss_task_index = 0;
    mmcss_handle_ =
        AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task_index);
    if (mmcss_handle_) {
        AvSetMmThreadPriority(mmcss_handle_, AVRT_PRIORITY_HIGH);
        active_ = true;
        std::printf("HostSim: Windows realtime session enabled (1 ms timer, elevated priority)\n");
    } else if (active_) {
        std::printf("HostSim: Windows timer/priority boost enabled\n");
    }
#else
    active_ = false;
#endif
}

RealtimeSession::~RealtimeSession() {
#ifdef _WIN32
    if (mmcss_handle_) {
        AvRevertMmThreadCharacteristics(mmcss_handle_);
        mmcss_handle_ = nullptr;
    }
    if (timer_period_ms_ != 0) {
        timeEndPeriod(timer_period_ms_);
        timer_period_ms_ = 0;
    }
#endif
}

} // namespace hostsim
