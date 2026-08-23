#include <RTEAutomation/Mcp2221Gpio.h>

#include "Mcp2221Protocol.h"

#include <hidapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwchar>
#include <thread>

namespace RTEAutomation {
namespace {

constexpr unsigned short kVendorId = 0x04d8;
constexpr unsigned short kProductId = 0x00dd;

std::string Narrow(const wchar_t* value) {
    if (!value || !*value) return {};
    std::mbstate_t state{};
    const wchar_t* source = value;
    const std::size_t length = std::wcsrtombs(nullptr, &source, 0, &state);
    if (length == static_cast<std::size_t>(-1)) return {};
    std::string result(length, '\0');
    state = {};
    source = value;
    std::wcsrtombs(result.data(), &source, result.size(), &state);
    return result;
}

std::string HidFailure(hid_device* device, const std::string& operation) {
    const std::string detail = Narrow(hid_error(device));
    return detail.empty() ? operation : operation + ": " + detail;
}

class HidSession final {
public:
    HidSession() = default;
    HidSession(const HidSession&) = delete;
    HidSession& operator=(const HidSession&) = delete;

    ~HidSession() {
        if (device_) hid_close(device_);
        if (initialized_) hid_exit();
    }

    bool Open(std::string& error) {
        if (hid_init() != 0) {
            error = "could not initialize HIDAPI";
            return false;
        }
        initialized_ = true;

        hid_device_info* devices = hid_enumerate(kVendorId, kProductId);
        if (!devices) {
            error = "MCP2221A was not found on USB";
            return false;
        }
        for (hid_device_info* current = devices; current; current = current->next) {
            device_ = hid_open_path(current->path);
            if (device_) break;
        }
        hid_free_enumeration(devices);
        if (!device_) {
            error = HidFailure(nullptr,
                "MCP2221A was found but could not be opened; check USB permissions");
            return false;
        }
        return true;
    }

    bool Exchange(const Mcp2221Protocol::Report& request,
                  Mcp2221Protocol::Report& response, std::string& error) {
        std::array<unsigned char, 65> output{};
        std::copy(request.begin(), request.end(), output.begin() + 1);

        for (int attempt = 0; attempt < 2; ++attempt) {
            const int written = hid_write(device_, output.data(), output.size());
            if (written != static_cast<int>(output.size())) {
                error = HidFailure(device_, "could not write MCP2221A HID report");
                continue;
            }
            const int read = hid_read_timeout(device_, response.data(), response.size(), 2000);
            if (read == static_cast<int>(response.size())) return true;
            error = read == 0
                ? "timed out waiting for MCP2221A HID response"
                : HidFailure(device_, "could not read MCP2221A HID response");
        }
        return false;
    }

private:
    hid_device* device_ = nullptr;
    bool initialized_ = false;
};

}  // namespace

Mcp2221GpioResult ControlMcp2221Gpio(Mcp2221GpioAction action) {
    HidSession session;
    std::string error;
    if (!session.Open(error)) return {false, std::move(error)};
    const bool success = Mcp2221Protocol::Execute(
        action,
        [&session](const Mcp2221Protocol::Report& request,
                   Mcp2221Protocol::Report& response, std::string& exchangeError) {
            return session.Exchange(request, response, exchangeError);
        },
        [](std::chrono::milliseconds duration) { std::this_thread::sleep_for(duration); },
        error);
    return {success, std::move(error)};
}

}  // namespace RTEAutomation
