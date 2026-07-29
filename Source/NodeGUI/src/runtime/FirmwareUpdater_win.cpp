#include "FirmwareUpdater.h"

#include <cstdio>

namespace NodeGUI::runtime {

// Windows stub: upstream FirmwareUpdater is Linux-first (popen/readlink/MCP2221).
// Flashing on Windows can still use Images/NucleoL476FW/scripts/flash.ps1.

FirmwareUpdater::FirmwareUpdater() {
    thread_ = std::thread([this] {
        while (run_.load()) {
            FlashJob job;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait_for(lk, std::chrono::milliseconds(200), [this] {
                    return !run_.load() || !queue_.empty();
                });
                if (!run_.load()) break;
                if (queue_.empty()) continue;
                job = queue_.front();
                queue_.pop_front();
                busy_ = true;
                state_ = FlashState::Failed;
                last_error_ = "FirmwareUpdater: not implemented on Windows "
                              "(use OpenOCD / flash.ps1)";
                log_.push_back(last_error_);
                busy_ = false;
            }
            (void)job;
        }
    });
}

FirmwareUpdater::~FirmwareUpdater() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        run_.store(false);
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool FirmwareUpdater::queueFlash(const FlashJob& job, bool allow_queue) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (busy_ && !allow_queue) return false;
    queue_.push_back(job);
    cv_.notify_all();
    return true;
}

FlashStatus FirmwareUpdater::status() const {
    std::lock_guard<std::mutex> lk(mtx_);
    FlashStatus s;
    s.state = state_;
    s.busy = busy_;
    s.last_error = last_error_;
    s.log = log_;
    return s;
}

void FirmwareUpdater::setCurrentPort(const std::string& port) {
    std::lock_guard<std::mutex> lk(mtx_);
    current_port_ = port;
}

std::string FirmwareUpdater::currentPort() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return current_port_;
}

void FirmwareUpdater::setSuspendCallback(std::function<void(bool)> cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    suspend_cb_ = std::move(cb);
}

const char* FirmwareUpdater::stateString(FlashState s) {
    switch (s) {
    case FlashState::Idle: return "Idle";
    case FlashState::Draining: return "Draining";
    case FlashState::EnteringBoot: return "EnteringBoot";
    case FlashState::Flashing: return "Flashing";
    case FlashState::Verifying: return "Verifying";
    case FlashState::ExitingBoot: return "ExitingBoot";
    case FlashState::Done: return "Done";
    case FlashState::Failed: return "Failed";
    }
    return "?";
}

void FirmwareUpdater::threadMain() {}
void FirmwareUpdater::runJob(const FlashJob&) {}
bool FirmwareUpdater::findCli(std::string&) const { return false; }
bool FirmwareUpdater::findGpioHelper(std::string&) const { return false; }
void FirmwareUpdater::setState(FlashState s) { state_ = s; }
void FirmwareUpdater::logLine(const std::string& line) { log_.push_back(line); }
void FirmwareUpdater::logStderr(const char*) {}
bool FirmwareUpdater::drainSerial(const std::string&) { return false; }
bool FirmwareUpdater::runCommand(const std::string&, const std::string&) { return false; }
bool FirmwareUpdater::gpioCommand(const std::string&, const std::string&) { return false; }

} // namespace NodeGUI::runtime
