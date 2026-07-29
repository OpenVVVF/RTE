#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hostsim {

/* TCP server that publishes InverterProtocol COBS frames for NodeGUI.
 * Accepts text shell lines (throttle / help / quit) from the same socket. */
class TelemetryPublisher {
public:
    static constexpr int kDefaultPort = 14608;

    TelemetryPublisher();
    ~TelemetryPublisher();

    TelemetryPublisher(const TelemetryPublisher&) = delete;
    TelemetryPublisher& operator=(const TelemetryPublisher&) = delete;

    bool Start(const std::string& host, int port);
    void Stop();
    bool IsListening() const;
    bool HasClient() const;

    /* Register / update a float telemetry key (DEFINE sent on next flush). */
    void LogF32(const char* key, float value);

    /* Built-in plant signals published every PublishCycle. */
    void SetBuiltin(float throttle_a, float throttle_b,
                    float duty_u, float duty_v, float duty_w,
                    float i_a, float i_b, float i_c,
                    float theta_e, float omega_e, float vdc);

    /* Send DEFINE (if dirty) + DATA to connected clients. */
    void PublishCycle(uint32_t time_us);

    /* DATA packet with only keys starting with prefix (for high-rate PWM scope). */
    void PublishPrefixCycle(uint32_t time_us, const char* key_prefix);

    /* DATA packet excluding keys with prefix (plant signals only). */
    void PublishPlantCycle(uint32_t time_us, const char* exclude_prefix);

    /* Non-blocking: read and process text commands from clients.
     * Returns true if the live loop should continue. */
    bool PollCommands();

    /* Optional overrides from console commands. */
    bool HasThrottleOverrideA() const;
    bool HasThrottleOverrideB() const;
    float ThrottleOverrideA() const;
    float ThrottleOverrideB() const;
    bool HasDutyOverrideU() const;
    bool HasDutyOverrideV() const;
    bool HasDutyOverrideW() const;
    float DutyOverrideU() const;
    float DutyOverrideV() const;
    float DutyOverrideW() const;
    void ClearThrottleOverrides();
    void ClearDutyOverrides();

    bool IsPaused() const { return paused_; }
    void SetPaused(bool paused) { paused_ = paused; }

    bool QuitRequested() const { return quit_requested_; }

private:
    struct Client;

    bool AcceptPending();
    void DropClient(size_t index);
    bool SendFramed(Client& c, const uint8_t* packet, size_t len);
    bool SendDefine(Client& c, uint32_t time_us);
    bool SendData(Client& c, uint32_t time_us);
    bool SendDataPrefix(Client& c, uint32_t time_us, const char* key_prefix);
    bool SendDataExcludePrefix(Client& c, uint32_t time_us, const char* exclude_prefix);
    void HandleLine(const std::string& line);
    void EnsureBuiltinIds();

    struct Signal {
        uint16_t id = 0;
        float value = 0.0f;
        bool defined = false;
    };

    std::string host_ = "127.0.0.1";
    int port_ = kDefaultPort;
    int listen_fd_ = -1;
    std::vector<Client> clients_;

    mutable std::mutex mu_;
    std::unordered_map<std::string, Signal> signals_;
    uint16_t next_id_ = 1;
    bool define_dirty_ = true;
    uint32_t seq_ = 0;

    bool throttle_a_override_ = false;
    bool throttle_b_override_ = false;
    float throttle_a_ = 0.0f;
    float throttle_b_ = 0.0f;
    bool duty_u_override_ = false;
    bool duty_v_override_ = false;
    bool duty_w_override_ = false;
    float duty_u_ = 0.0f;
    float duty_v_ = 0.0f;
    float duty_w_ = 0.0f;
    bool quit_requested_ = false;
    bool paused_ = false;
};

TelemetryPublisher& GlobalTelemetryPublisher();

} // namespace hostsim
