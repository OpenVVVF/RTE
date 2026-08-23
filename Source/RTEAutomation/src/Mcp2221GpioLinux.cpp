#include <RTEAutomation/Mcp2221Gpio.h>

#include <linux/gpio.h>

#include <sys/ioctl.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace RTEAutomation {
namespace {

namespace fs = std::filesystem;

constexpr char kChipLabel[] = "mcp2221_gpio";
constexpr std::uint64_t kBothLines = 0x3;
constexpr std::uint64_t kResetHigh = 0x2;

std::string SystemError(const std::string& operation) {
    return operation + ": " + std::strerror(errno);
}

class FileDescriptor final {
public:
    FileDescriptor() = default;
    explicit FileDescriptor(int fd) : fd_(fd) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~FileDescriptor() {
        if (fd_ >= 0) close(fd_);
    }

    [[nodiscard]] int Get() const { return fd_; }
    [[nodiscard]] bool Valid() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

bool KernelMcp2221IsPresent() {
    std::error_code error;
    const fs::path gpioClass("/sys/class/gpio");
    for (fs::directory_iterator it(gpioClass, error), end; !error && it != end;
         it.increment(error)) {
        std::ifstream label(it->path() / "label");
        std::string value;
        if (label >> value; value == kChipLabel) return true;
    }
    return false;
}

FileDescriptor OpenMcp2221(std::string& error) {
    std::error_code directoryError;
    std::vector<fs::path> candidates;
    for (fs::directory_iterator it("/dev", directoryError), end;
         !directoryError && it != end; it.increment(directoryError)) {
        const std::string name = it->path().filename().string();
        if (name.starts_with("gpiochip")) candidates.push_back(it->path());
    }
    std::sort(candidates.begin(), candidates.end());

    bool permissionDenied = false;
    for (const auto& candidate : candidates) {
        FileDescriptor chip(open(candidate.c_str(), O_RDONLY | O_CLOEXEC));
        if (!chip.Valid()) {
            permissionDenied = permissionDenied || errno == EACCES || errno == EPERM;
            continue;
        }
        gpiochip_info info{};
        if (ioctl(chip.Get(), GPIO_GET_CHIPINFO_IOCTL, &info) == 0
            && std::string_view(info.label) == kChipLabel) {
            if (info.lines < 2) {
                error = "MCP2221A GPIO device exposes fewer than two pins";
                return {};
            }
            return chip;
        }
    }

    if (KernelMcp2221IsPresent() && permissionDenied) {
        error = "MCP2221A was found but its Linux GPIO device is not accessible; "
                "install 60-rte-mcp2221.rules from share/rte/udev (or "
                "packaging/udev in the source tree), reload udev rules, and reconnect it";
    } else if (KernelMcp2221IsPresent()) {
        error = "MCP2221A kernel GPIO device exists but no usable /dev/gpiochip node was found";
    } else {
        error = "MCP2221A was not found on USB";
    }
    return {};
}

class LineRequest final {
public:
    bool Open(int chip, bool output, std::string& error) {
        gpio_v2_line_request request{};
        request.offsets[0] = 0;
        request.offsets[1] = 1;
        request.num_lines = 2;
        std::strncpy(request.consumer, "rte", sizeof(request.consumer) - 1);
        request.config.flags = output
            ? GPIO_V2_LINE_FLAG_OUTPUT
            : GPIO_V2_LINE_FLAG_INPUT;
        if (output) {
            request.config.num_attrs = 1;
            request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
            request.config.attrs[0].attr.values = kResetHigh;
            request.config.attrs[0].mask = kBothLines;
        }
        if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
            error = SystemError("could not request MCP2221A GP0/GP1");
            return false;
        }
        lines_ = FileDescriptor(request.fd);
        return true;
    }

    bool Set(bool boot0, bool reset, std::string& error) const {
        gpio_v2_line_values values{};
        values.mask = kBothLines;
        values.bits = (boot0 ? 0x1 : 0) | (reset ? 0x2 : 0);
        if (ioctl(lines_.Get(), GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0) {
            error = SystemError("could not set MCP2221A GP0/GP1");
            return false;
        }
        return true;
    }

private:
    FileDescriptor lines_;
};

}  // namespace

Mcp2221GpioResult ControlMcp2221Gpio(Mcp2221GpioAction action) {
    std::string error;
    auto chip = OpenMcp2221(error);
    if (!chip.Valid()) return {false, std::move(error)};

    LineRequest lines;
    const bool release = action == Mcp2221GpioAction::ReleasePins;
    if (!lines.Open(chip.Get(), !release, error)) return {false, std::move(error)};
    if (release) return {true, {}};

    using namespace std::chrono_literals;
    std::this_thread::sleep_for(10ms);
    if (action == Mcp2221GpioAction::EnterBootloader) {
        if (!lines.Set(true, true, error)) return {false, std::move(error)};
        std::this_thread::sleep_for(50ms);
        if (!lines.Set(true, false, error)) return {false, std::move(error)};
        std::this_thread::sleep_for(50ms);
        if (!lines.Set(true, true, error)) return {false, std::move(error)};
        std::this_thread::sleep_for(250ms);
    } else {
        if (!lines.Set(false, false, error)) return {false, std::move(error)};
        std::this_thread::sleep_for(50ms);
        if (!lines.Set(false, true, error)) return {false, std::move(error)};
        std::this_thread::sleep_for(100ms);
    }
    return {true, {}};
}

}  // namespace RTEAutomation
