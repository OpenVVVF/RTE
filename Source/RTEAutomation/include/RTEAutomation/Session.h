#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace RTEAutomation {

struct SessionDescriptor {
    int version = 1;
    std::string app;
    std::string host = "127.0.0.1";
    int port = 0;
    std::string token;
    std::uint64_t pid = 0;
};

std::filesystem::path SessionDirectory();
std::filesystem::path CurrentSessionPath();
bool WriteSessionDescriptor(const SessionDescriptor& descriptor,
                            std::string* error = nullptr);
void RemoveSessionDescriptor(std::uint64_t expectedPid = 0);
std::optional<SessionDescriptor> DiscoverSession(
    const std::filesystem::path& descriptorPath = {},
    std::string* error = nullptr);

// Performs one authenticated newline-delimited JSON request to RTE Studio.
// The returned JSON is the value in the server's `result` member. Server and
// transport failures are reported through error.
std::optional<nlohmann::json> RequestSession(
    const SessionDescriptor& descriptor,
    const std::string& method,
    const nlohmann::json& params,
    std::string* error = nullptr);

}  // namespace RTEAutomation
