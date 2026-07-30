#include "FirmwareUpdater.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <system_error>

namespace NodeGUI::runtime {

FirmwareUpdater::FirmwareUpdater() {
    thread_ = std::thread([this] { threadMain(); });
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

void FirmwareUpdater::setState(FlashState s) {
    std::lock_guard<std::mutex> lk(mtx_);
    state_ = s;
}

void FirmwareUpdater::logLine(const std::string& line) {
    std::lock_guard<std::mutex> lk(mtx_);
    log_.push_back(line);
}

void FirmwareUpdater::logStderr(const char* buf) {
    if (!buf) return;
    std::lock_guard<std::mutex> lk(mtx_);
    log_.push_back(std::string(buf));
}

bool FirmwareUpdater::drainSerial(const std::string&) {
    return true;
}

bool FirmwareUpdater::findCli(std::string& out_cli) const {
    // Look for openocd.exe or STM32CubeIDE installation
    const char* env_path = std::getenv("OPENOCD_PATH");
    if (env_path && std::filesystem::exists(env_path)) {
        out_cli = env_path;
        return true;
    }
    out_cli = "openocd.exe";
    return true;
}

bool FirmwareUpdater::findGpioHelper(std::string& out_helper) const {
    out_helper.clear();
    return false;
}

bool FirmwareUpdater::runCommand(const std::string& cmd, const std::string& desc) {
    logLine("[Windows] Running: " + desc);
    logLine(" > " + cmd);

    FILE* rawPipe = _popen(cmd.c_str(), "r");
    if (!rawPipe) {
        std::lock_guard<std::mutex> lk(mtx_);
        last_error_ = "Failed to launch process for " + desc;
        log_.push_back("[error] " + last_error_);
        return false;
    }

    std::unique_ptr<FILE, decltype(&_pclose)> pipe(rawPipe, _pclose);

    std::array<char, 256> buffer;
    std::string current_line;
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        for (char c : std::string_view(buffer.data())) {
            if (c == '\n' || c == '\r') {
                if (!current_line.empty()) {
                    logLine(current_line);
                    current_line.clear();
                }
            } else {
                current_line += c;
            }
        }
    }
    if (!current_line.empty()) {
        logLine(current_line);
    }

    int ret = pipe.release() ? _pclose(rawPipe) : 0;
    if (ret != 0) {
        std::lock_guard<std::mutex> lk(mtx_);
        last_error_ = desc + " failed with code " + std::to_string(ret);
        log_.push_back("[error] " + last_error_);
        return false;
    }
    return true;
}

bool FirmwareUpdater::gpioCommand(const std::string&, const std::string&) {
    return true;
}

void FirmwareUpdater::runJob(const FlashJob& job) {
    setState(FlashState::Flashing);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        last_error_.clear();
        log_.clear();
    }

    logLine("============================================================");
    logLine("Starting Windows Firmware Flash Job");
    logLine("Target Path: " + job.firmware_path);
    logLine("============================================================");

    std::function<void(bool)> cb;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cb = suspend_cb_;
    }
    if (cb) cb(true);

    std::string fwPath = job.firmware_path;
    if (fwPath.empty()) {
        fwPath = "build/nucleo_emitted_build/STM32CubeMX.elf";
    }

    // Sanitize firmware path for PowerShell argument
    std::string cleanFwPath;
    for (char c : fwPath) {
        if (c != '"' && c != '\'' && c != '`') cleanFwPath += c;
    }

    std::string flashCmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File Images\\NucleoL476FW\\scripts\\flash.ps1 -Elf \"" + cleanFwPath + "\"";

    bool ok = runCommand(flashCmd, "Flash Firmware to STM32 (OpenOCD)");

    if (cb) cb(false);

    if (ok) {
        setState(FlashState::Done);
        logLine("[success] Firmware flashed and verified successfully!");
    } else {
        setState(FlashState::Failed);
    }
}

void FirmwareUpdater::threadMain() {
    while (run_.load()) {
        FlashJob job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(100), [this] {
                return !run_.load() || !queue_.empty();
            });
            if (!run_.load()) break;
            if (queue_.empty()) continue;
            job = queue_.front();
            queue_.pop_front();
            busy_ = true;
        }

        try {
            runJob(job);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(mtx_);
            last_error_ = std::string("Unhandled exception in flash worker: ") + e.what();
            log_.push_back("[error] " + last_error_);
            state_ = FlashState::Failed;
        } catch (...) {
            std::lock_guard<std::mutex> lk(mtx_);
            last_error_ = "Unknown exception in flash worker thread";
            log_.push_back("[error] " + last_error_);
            state_ = FlashState::Failed;
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            busy_ = false;
        }
    }
}

} // namespace NodeGUI::runtime
