#pragma once

namespace hostsim {

/* RAII helper: raises Windows timer resolution and thread priority for the
 * live simulation loop. No-op on other platforms. */
class RealtimeSession {
public:
    RealtimeSession();
    ~RealtimeSession();

    RealtimeSession(const RealtimeSession&) = delete;
    RealtimeSession& operator=(const RealtimeSession&) = delete;

    bool Active() const { return active_; }

private:
    bool active_ = false;
#ifdef _WIN32
    unsigned int timer_period_ms_ = 0;
    void* mmcss_handle_ = nullptr;
#endif
};

} // namespace hostsim
