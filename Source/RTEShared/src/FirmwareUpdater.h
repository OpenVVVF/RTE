#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rte::runtime {

enum class FlashState {
    Idle,
    Draining,
    EnteringBoot,
    Flashing,
    Verifying,
    ExitingBoot,
    Done,
    Failed
};

struct FlashStatus {
    FlashState state = FlashState::Idle;
    bool busy = false;
    std::string last_error;
    std::vector<std::string> log;
    // Flash progress 0..100, or -1 when indeterminate (idle, drains, GPIO
    // waits) / nothing has run yet.
    int progress = -1;
};

struct FlashJob {
    std::string firmware_path;
    std::string port;
    bool auto_gpio = true;
    bool delete_after_flash = false;  // set for temp files from HTTP
};

// Qt-free firmware flash worker, ported from the old ImGui client's
// FirmwareUpdater. One worker thread, one job at a time. Workflow per job:
// suspend telemetry (callback) -> find STM32_Programmer_CLI -> drain serial ->
// mcp2221a_gpio.py enter -> drain again -> CLI flash + verify ->
// mcp2221a_gpio.py exit + release -> resume telemetry.
class FirmwareUpdater {
public:
    FirmwareUpdater();
    ~FirmwareUpdater();

    FirmwareUpdater(const FirmwareUpdater&) = delete;
    FirmwareUpdater& operator=(const FirmwareUpdater&) = delete;

    // Queue a flash job. Returns false if a job is already running and
    // `allow_queue` is false. HTTP requests set allow_queue=false so they can
    // be rejected while busy.
    bool queueFlash(const FlashJob& job, bool allow_queue = false);

    // Thread-safe status snapshot.
    FlashStatus status() const;

    // Port used by HTTP-triggered flashes.
    void setCurrentPort(const std::string& port);
    std::string currentPort() const;

    // Callback to pause/resume the telemetry client while flashing so the
    // serial port is not contended. argument true = suspend, false = resume.
    void setSuspendCallback(std::function<void(bool)> cb);

    // Human-readable state string.
    static const char* stateString(FlashState s);

private:
    void threadMain();
    void runJob(const FlashJob& job);

    bool findCli(std::string& out_cli) const;
    bool findGpioHelper(std::string& out_helper) const;

    void setState(FlashState s);
    void setProgress(int percent);
    void logLine(const std::string& line);
    void logStderr(const char* buf);

    bool drainSerial(const std::string& port);
    bool runCommand(const std::string& cmd, const std::string& desc);
    bool gpioCommand(const std::string& helper, const std::string& arg);

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<FlashJob> queue_;
    FlashState state_ = FlashState::Idle;
    std::vector<std::string> log_;
    std::string last_error_;
    std::string current_port_;
    std::function<void(bool)> suspend_cb_;
    bool busy_ = false;
    int progress_ = -1;

    std::atomic<bool> run_{true};
    std::thread thread_;
};

}  // namespace rte::runtime

namespace NodeGUI::runtime {
using rte::runtime::FirmwareUpdater;
using rte::runtime::FlashJob;
using rte::runtime::FlashState;
using rte::runtime::FlashStatus;
}
