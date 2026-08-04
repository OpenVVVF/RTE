// Firmware flashing worker, ported from the old ImGui client
// (InverterClientImGui/src/firmware_updater.cpp) to Qt-free std C++ for
// NodeGUI. Linux only (the old _WIN32 paths were dropped).
//
// Deliberate changes vs. the old file:
//   * Namespace NodeGUI::runtime; class shape follows FirmwareUpdater.h.
//   * drainSerial uses the in-repo ivp::SerialPort instead of the old app's
//     own SerialPort.
//   * The GPIO helper search additionally honors the RTE_TOOLS_DIR compile
//     definition (searched first).
//   * Two old bugs fixed (marked at the relevant sites below):
//       1. busy_ was written outside the mutex on several exit paths —
//          all writes are now under mtx_ (last_error_ writes too, same race).
//       2. The "Verifying" state was cosmetic (entered unconditionally with
//          a pre-printed success line). Verifying is now only entered while
//          the CLI's -v phase actually runs, and its real result is reported.

#include "FirmwareUpdater.h"

#include <inverter_protocol/host/uart_transport.h>

#include <cctype>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

namespace NodeGUI::runtime {
namespace {

constexpr uint32_t FLASH_BAUDRATE = 230400;
constexpr const char* FLASH_ADDR = "0x08000000";

// Strip ANSI escape sequences (ESC [ ... final-byte) and non-printable
// characters from CLI output so the log stays readable. STM32_Programmer_CLI
// colors parts of its output even when piped.
std::string sanitizeLine(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == 0x1b && i + 1 < raw.size() && raw[i + 1] == '[') {
            i += 2;
            while (i < raw.size()) {
                const unsigned char f = static_cast<unsigned char>(raw[i]);
                ++i;
                if (f >= 0x40 && f <= 0x7e) break;  // CSI final byte
            }
            continue;
        }
        if (c >= 0x20 || c == '\t') out.push_back(static_cast<char>(c));
        ++i;
    }
    return out;
}

// The CLI redraws progress in place using carriage returns: only the LAST
// '\r'-separated segment of a line carries current information.
std::string lastCarriageSegment(const std::string& line) {
    const size_t pos = line.find_last_of('\r');
    return pos == std::string::npos ? line : line.substr(pos + 1);
}

// Split a chunk of command output into lines, invoking f for each usable
// line: '\n'-separated, carriage-return redraws collapsed to their last
// segment, ANSI escapes and control characters stripped.
template <typename F>
void forEachOutputLine(const char* buf, F&& f) {
    std::string s(buf ? buf : "");
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find('\n', start);
        if (end == std::string::npos) end = s.size();
        std::string line = sanitizeLine(lastCarriageSegment(s.substr(start, end - start)));
        if (!line.empty()) f(line);
        start = end + 1;
    }
}

// Last "NN%" in a line, or -1 when there is none. The CLI prints its download
// progress as percentages (sometimes several on one line; the last is newest).
int parsePercent(const std::string& line) {
    const size_t pct = line.find_last_of('%');
    if (pct == std::string::npos || pct == 0) return -1;
    size_t begin = pct;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(line[begin - 1]))) --begin;
    if (begin == pct) return -1;
    return std::atoi(line.substr(begin, pct - begin).c_str());
}

// popen-based command runner: streams the command's output chunk by chunk to
// on_chunk and reports the exit status. On failure out_error is set.
bool runCommandStreaming(const std::string& cmd, const std::string& desc,
                         const std::function<void(const char*)>& on_chunk,
                         std::string& out_error) {
    // STM32_Programmer_CLI uses both output streams. Merge stderr into the
    // pipe so remote clients receive diagnostics as well as normal output.
    FILE* f = popen((cmd + " 2>&1").c_str(), "r");
    if (!f) {
        out_error = desc + ": failed to run command";
        return false;
    }
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
        if (on_chunk) on_chunk(buf);
    }
    int rc = pclose(f);
    if (rc != 0) {
        out_error = desc + ": command exited with code " + std::to_string(rc);
        return false;
    }
    return true;
}

fs::path exeDir() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return fs::path(buf).parent_path();
    }
    return {};
}

// Find a python interpreter that has EasyMCP2221 installed. Prefers the
// project .venv (created with: python3 -m venv .venv && .venv/bin/pip
// install EasyMCP2221); falls back to plain python3.
std::string findPython() {
    std::error_code ec;

    if (const char* env = std::getenv("RTE_PYTHON")) {
        if (fs::is_regular_file(env, ec)) return env;
    }

    fs::path exe_dir = exeDir();
    std::vector<fs::path> candidates;
    const char* venv_py[] = {".venv/bin/python"};
    for (const char* rel : venv_py) {
        if (!exe_dir.empty()) {
            candidates.push_back(exe_dir / rel);
            candidates.push_back(exe_dir.parent_path() / rel);
        }
        candidates.push_back(fs::current_path(ec) / rel);
        candidates.push_back(rel);
    }

    for (const auto& p : candidates) {
        if (fs::is_regular_file(p, ec)) {
            // Do NOT canonicalize: a venv python is a symlink to the system
            // interpreter, and resolving it would drop the venv site-packages.
            std::string c = fs::absolute(p, ec).string();
            return c.empty() ? p.string() : c;
        }
    }
    return "python3";
}

void callSuspendCb(const std::function<void(bool)>& cb, bool suspend) {
    if (cb) {
        try {
            cb(suspend);
        } catch (...) {
            // ignore callback errors; flash can still proceed
        }
    }
}

// RAII guard: suspends telemetry on construction and always resumes it on
// destruction, however runJob exits.
struct SuspendGuard {
    std::function<void(bool)> cb;
    explicit SuspendGuard(std::function<void(bool)> cb_) : cb(std::move(cb_)) {
        callSuspendCb(cb, true);
    }
    ~SuspendGuard() { callSuspendCb(cb, false); }
    SuspendGuard(const SuspendGuard&) = delete;
    SuspendGuard& operator=(const SuspendGuard&) = delete;
};

}  // namespace

FirmwareUpdater::FirmwareUpdater() {
    thread_ = std::thread(&FirmwareUpdater::threadMain, this);
}

FirmwareUpdater::~FirmwareUpdater() {
    run_.store(false);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool FirmwareUpdater::queueFlash(const FlashJob& job, bool allow_queue) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (busy_) {
            if (!allow_queue) return false;
            // queue up to one pending job
            if (!queue_.empty()) return false;
            queue_.push_back(job);
            cv_.notify_all();
            return true;
        }
        queue_.push_back(job);
        // Mark busy NOW, not when the worker picks the job up: previously a
        // second queueFlash in that window raced in another job and the
        // firmware got flashed twice.
        busy_ = true;
        cv_.notify_all();
    }
    return true;
}

FlashStatus FirmwareUpdater::status() const {
    std::lock_guard<std::mutex> lk(mtx_);
    FlashStatus s;
    s.state = state_;
    s.busy = busy_;
    s.last_error = last_error_;
    s.log = log_;
    s.progress = progress_;
    return s;
}

const char* FirmwareUpdater::stateString(FlashState s) {
    switch (s) {
        case FlashState::Idle:          return "Idle";
        case FlashState::Draining:      return "Draining serial";
        case FlashState::EnteringBoot:  return "Entering bootloader";
        case FlashState::Flashing:      return "Flashing";
        case FlashState::Verifying:     return "Verifying";
        case FlashState::ExitingBoot:   return "Exiting bootloader";
        case FlashState::Done:          return "Done";
        case FlashState::Failed:        return "Failed";
    }
    return "Unknown";
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

void FirmwareUpdater::setState(FlashState s) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = s;
        // Keep the progress bar meaningful per phase: determinate while the
        // CLI flashes/verifies, indeterminate otherwise, full when done.
        switch (s) {
            case FlashState::Flashing:
            case FlashState::Verifying:
                if (progress_ < 0) progress_ = 0;
                break;
            case FlashState::Done:
                progress_ = 100;
                break;
            case FlashState::Idle:
            case FlashState::Failed:
                progress_ = -1;
                break;
            default:
                progress_ = -1;
                break;
        }
    }
    logLine(std::string("[state] ") + stateString(s));
}

void FirmwareUpdater::setProgress(int percent) {
    std::lock_guard<std::mutex> lk(mtx_);
    progress_ = std::clamp(percent, 0, 100);
}

void FirmwareUpdater::logLine(const std::string& line) {
    std::lock_guard<std::mutex> lk(mtx_);
    log_.push_back(line);
}

void FirmwareUpdater::logStderr(const char* buf) {
    forEachOutputLine(buf, [this](const std::string& line) {
        // Progress bars / percentage updates carry no letters — they are
        // reported through the progress field instead of flooding the log.
        const bool hasLetter =
            std::any_of(line.begin(), line.end(), [](unsigned char c) { return std::isalpha(c); });
        if (hasLetter) {
            logLine(line);
        }
    });
}

bool FirmwareUpdater::findCli(std::string& out_cli) const {
    const char* candidates[] = {
        "/opt/st/stm32cubeclt_1.21.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI",
        "/opt/st/stm32cubeclt/STM32CubeProgrammer/bin/STM32_Programmer_CLI",
        "STM32_Programmer_CLI",
        nullptr
    };

    for (const char** p = candidates; *p; ++p) {
        std::error_code ec;
        fs::path path(*p);
        // If it has a directory component, check existence directly.
        if (path.has_parent_path()) {
            if (fs::exists(path, ec) && !fs::is_directory(path, ec)) {
                out_cli = fs::canonical(path, ec).string();
                if (out_cli.empty()) out_cli = path.string();
                return true;
            }
        } else {
            // Search PATH
            const char* path_env = std::getenv("PATH");
            if (path_env) {
                std::stringstream ss(path_env);
                std::string dir;
                while (std::getline(ss, dir, ':')) {
                    fs::path full = fs::path(dir) / path.filename();
                    if (fs::is_regular_file(full, ec)) {
                        out_cli = full.string();
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool FirmwareUpdater::findGpioHelper(std::string& out_helper) const {
    std::error_code ec;
    fs::path exe_dir = exeDir();

    std::vector<fs::path> candidates;
#ifdef RTE_TOOLS_DIR
    // Compile-time tools directory (set by the RTE build) is searched first.
    candidates.push_back(fs::path(RTE_TOOLS_DIR) / "mcp2221a_gpio.py");
#endif
    if (!exe_dir.empty()) {
        candidates.push_back(exe_dir / "tools" / "mcp2221a_gpio.py");
        candidates.push_back(exe_dir / "Tools" / "mcp2221a_gpio.py");
        candidates.push_back(exe_dir.parent_path() / "tools" / "mcp2221a_gpio.py");
        candidates.push_back(exe_dir.parent_path() / "Tools" / "mcp2221a_gpio.py");
    }
    candidates.push_back(fs::current_path(ec) / "tools" / "mcp2221a_gpio.py");
    candidates.push_back(fs::current_path(ec) / "Tools" / "mcp2221a_gpio.py");
    candidates.push_back("tools/mcp2221a_gpio.py");
    candidates.push_back("Tools/mcp2221a_gpio.py");

    for (const auto& p : candidates) {
        if (fs::is_regular_file(p, ec)) {
            out_helper = fs::canonical(p, ec).string();
            if (out_helper.empty()) out_helper = p.string();
            return true;
        }
    }
    return false;
}

bool FirmwareUpdater::drainSerial(const std::string& port) {
    setState(FlashState::Draining);
    ivp::SerialPort sp;
    // NOTE: ivp::SerialPort::open() ignores the baud argument and always
    // opens at its fixed 460800. The old ImGui app's SerialPort had the same
    // quirk, so the drain behavior is unchanged from the old client.
    if (!sp.open(port, static_cast<int>(FLASH_BAUDRATE))) {
        logLine("[drain] Failed to open port " + port);
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    uint8_t junk[4096];
    int total = 0;
    // 20 x 20 ms ~= 400 ms of drain reads, as in the old client.
    for (int i = 0; i < 20 && run_.load(); ++i) {
        int n = sp.read(junk, (int)sizeof(junk));
        if (n > 0) total += n;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    sp.close();
    logLine(std::string("[drain] Drained ") + std::to_string(total) + " stale byte(s)");
    return true;
}

bool FirmwareUpdater::runCommand(const std::string& cmd, const std::string& desc) {
    logLine(std::string("$ ") + cmd);
    std::string err;
    if (runCommandStreaming(cmd, desc,
                            [this](const char* chunk) { logStderr(chunk); },
                            err)) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        last_error_ = err;
    }
    logLine(err);
    return false;
}

bool FirmwareUpdater::gpioCommand(const std::string& helper, const std::string& arg) {
    static const std::string python = findPython();
    std::string cmd = "\"" + python + "\" \"" + helper + "\" " + arg;
    return runCommand(cmd, std::string("GPIO ") + arg);
}

void FirmwareUpdater::runJob(const FlashJob& job) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        busy_ = true;
        last_error_.clear();
        log_.clear();
        state_ = FlashState::Idle;
    }

    // Old bug (1) fixed: every write to busy_ / last_error_ below happens
    // under mtx_; the old code wrote busy_ unlocked on several exit paths,
    // racing with status() readers.
    auto set_busy = [this](bool b) {
        std::lock_guard<std::mutex> lk(mtx_);
        busy_ = b;
    };
    auto set_error = [this](const std::string& msg) {
        std::lock_guard<std::mutex> lk(mtx_);
        last_error_ = msg;
    };

    logLine("[telemetry] Suspending telemetry reader to free serial port");
    // Snapshot the callback under the mutex, then suspend for the whole job;
    // the guard always resumes telemetry, however we exit.
    std::function<void(bool)> suspend_cb;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        suspend_cb = suspend_cb_;
    }
    SuspendGuard sg(std::move(suspend_cb));

    logLine("==================================================");
    logLine(" Firmware update started");
    logLine(" File: " + job.firmware_path);
    logLine(" Port: " + job.port);
    logLine(" Mode: " + std::string(job.auto_gpio ? "AUTO (MCP2221A)" : "MANUAL"));
    logLine("==================================================");

    std::string cli;
    bool found = findCli(cli);
    if (!found) {
        const std::string msg =
            "STM32_Programmer_CLI not found. Install STM32CubeCLT.";
        set_error(msg);
        logLine("[ERROR] " + msg);
        setState(FlashState::Failed);
        set_busy(false);
        return;
    }
    logLine(std::string("[CLI] ") + cli);

    std::string gpio_helper;
    bool auto_mode = false;
    if (job.auto_gpio) {
        if (findGpioHelper(gpio_helper)) {
            logLine(std::string("[GPIO helper] ") + gpio_helper);
            auto_mode = true;
        } else {
            logLine("[WARN] MCP2221A GPIO helper not found; falling back to MANUAL mode.");
            logLine("       Place tools/mcp2221a_gpio.py next to the executable.");
        }
    }

    // 1. Drain serial before touching reset/BOOT0.
    drainSerial(job.port);

    // 2. Enter bootloader.
    setState(FlashState::EnteringBoot);
    if (auto_mode) {
        if (!gpioCommand(gpio_helper, "enter")) {
            logLine("[WARN] GPIO enter failed; continuing in MANUAL mode.");
            auto_mode = false;
        }
    } else {
        logLine("[MANUAL] 1. Hold BOOT0 HIGH");
        logLine("[MANUAL] 2. Press and release RESET");
        logLine("[MANUAL] Waiting 5 seconds for user...");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    // Drain again now that the ROM bootloader is running: the application may
    // have left kilobytes of telemetry buffered in the USB-UART bridge, which
    // would otherwise drown the bootloader's sync ACK.
    drainSerial(job.port);

    // 3. Flash + verify.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    setState(FlashState::Flashing);

    std::ostringstream cmd;
    cmd << "\"" << cli << "\" -c port=" << job.port << " br=" << FLASH_BAUDRATE
        << " -w \"" << job.firmware_path << "\" " << FLASH_ADDR << " -v";
    logLine(std::string("$ ") + cmd.str());

    // Old bug (2) fixed: the old code entered Verifying and printed
    // "[verify] Download verified successfully" unconditionally after the CLI
    // finished, so the state was cosmetic. Now Verifying is only entered when
    // the CLI actually announces its -v phase in its output, and success is
    // only reported when the CLI confirms it. The CLI exit code remains the
    // authority: a failed verify makes the command exit non-zero.
    bool verify_started = false;
    bool verify_ok = false;
    std::string flash_err;
    bool flash_ok = runCommandStreaming(
        cmd.str(), "Flash",
        [&](const char* chunk) {
            logStderr(chunk);
            forEachOutputLine(chunk, [&](const std::string& line) {
                if (const int pct = parsePercent(line); pct >= 0) {
                    setProgress(pct);
                }
                std::string lower;
                lower.reserve(line.size());
                for (char c : line)
                    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                if (!verify_started && lower.find("verif") != std::string::npos) {
                    verify_started = true;
                    setState(FlashState::Verifying);
                }
                if (lower.find("download verified successfully") != std::string::npos)
                    verify_ok = true;
            });
        },
        flash_err);

    if (!flash_ok) {
        set_error(flash_err);
        logLine(flash_err);
        setState(FlashState::Failed);
        if (auto_mode) gpioCommand(gpio_helper, "release");
        set_busy(false);
        return;
    }

    if (verify_started) {
        if (verify_ok) {
            logLine("[verify] Download verified successfully");
        } else {
            logLine("[verify] WARNING: CLI exited 0 but verification success "
                    "was not confirmed in its output");
        }
    } else {
        logLine("[verify] NOTE: no verification banner seen in CLI output; "
                "relying on CLI exit code 0");
    }

    // 4. Exit bootloader.
    setState(FlashState::ExitingBoot);
    if (auto_mode) {
        gpioCommand(gpio_helper, "exit");
        gpioCommand(gpio_helper, "release");
    } else {
        logLine("[MANUAL] 1. Release BOOT0 (pull LOW)");
        logLine("[MANUAL] 2. Press RESET to run application");
    }

    setState(FlashState::Done);
    logLine("Firmware update complete.");
    set_busy(false);

    if (job.delete_after_flash) {
        std::error_code ec;
        fs::remove(job.firmware_path, ec);
    }
}

void FirmwareUpdater::threadMain() {
    while (run_.load()) {
        FlashJob job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&] { return !run_.load() || !queue_.empty(); });
            if (!run_.load()) break;
            if (queue_.empty()) continue;
            job = queue_.front();
            queue_.pop_front();
        }
        runJob(job);
    }
}

}  // namespace NodeGUI::runtime
