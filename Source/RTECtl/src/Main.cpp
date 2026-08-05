#include <GatewayClient.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

using json = nlohmann::json;

namespace {

volatile std::sig_atomic_t gStop = 0;
void OnSignal(int) { gStop = 1; }

void Usage() {
    std::cerr
        << "usage: rtectl [--server URL] [--lease ID] <command> [args]\n"
        << "  info                         gateway and device information\n"
        << "  status                       current telemetry/lease/flash state\n"
        << "  watch                        follow telemetry and console events\n"
        << "  console [--since N]          print filtered console history\n"
        << "  command <line...>            acquire control and run a command\n"
        << "  flash <firmware.bin>         acquire control, flash, and follow status\n"
        << "  control status|hold|release  inspect or explicitly manage control\n";
}

void PrettyPrint(const std::string& text) {
    const json value = json::parse(text, nullptr, false);
    std::cout << (value.is_discarded() ? text : value.dump(2)) << '\n';
}

struct AutoLease {
    rte::runtime::GatewayClient& client;
    bool acquired = false;
    ~AutoLease() { if (acquired) client.releaseLease(); }
};

bool EnsureLease(rte::runtime::GatewayClient& client, AutoLease& lease) {
    if (client.hasLease()) return true;
    std::string error;
    lease.acquired = client.acquireLease("rtectl", &error);
    if (!lease.acquired) std::cerr << "rtectl: " << error << '\n';
    return lease.acquired;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string server = "http://127.0.0.1:18080";
    std::string suppliedLease;
    int index = 1;
    while (index < argc) {
        const std::string argument = argv[index];
        if (argument == "--server" && index + 1 < argc) {
            server = argv[index + 1];
            index += 2;
        } else if (argument == "--lease" && index + 1 < argc) {
            suppliedLease = argv[index + 1];
            index += 2;
        } else {
            break;
        }
    }
    if (index >= argc) {
        Usage();
        return 1;
    }

    rte::runtime::GatewayClient client(server);
    if (!suppliedLease.empty()) client.useLease(suppliedLease);
    const std::string command = argv[index++];
    std::string error;

    if (command == "info") {
        const std::string body = client.infoJson(&error);
        if (body.empty()) { std::cerr << "rtectl: " << error << '\n'; return 1; }
        PrettyPrint(body);
        return 0;
    }
    if (command == "status") {
        const std::string body = client.stateJson(&error);
        if (body.empty()) { std::cerr << "rtectl: " << error << '\n'; return 1; }
        PrettyPrint(body);
        return 0;
    }
    if (command == "console") {
        uint64_t since = 0;
        if (index + 1 < argc && std::string(argv[index]) == "--since") {
            since = std::stoull(argv[index + 1]);
        }
        const std::string body = client.consoleJson(since, 6000, &error);
        if (body.empty()) { std::cerr << "rtectl: " << error << '\n'; return 1; }
        const json parsed = json::parse(body, nullptr, false);
        for (const auto& line : parsed.value("lines", json::array())) {
            std::cout << line.value("text", "") << '\n';
        }
        return 0;
    }
    if (command == "watch") {
        std::signal(SIGINT, OnSignal);
        std::signal(SIGTERM, OnSignal);
        client.onF32 = [](const auto& name, float value, float t) {
            std::cout << t << ' ' << name << '=' << value << '\n';
        };
        client.onString = [](const auto& name, const auto& value) {
            std::cout << name << "=\"" << value << "\"\n";
        };
        client.onConsole = [](uint64_t, const auto& line) {
            std::cout << "console: " << line << '\n';
        };
        client.onConnection = [](bool connected, const auto& detail) {
            std::cerr << (connected ? "connected" : "disconnected")
                      << (detail.empty() ? "" : ": " + detail) << '\n';
        };
        client.startEvents();
        while (!gStop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        client.stopEvents();
        return 0;
    }
    if (command == "command") {
        if (index >= argc) { Usage(); return 1; }
        std::string line;
        for (; index < argc; ++index) {
            if (!line.empty()) line += ' ';
            line += argv[index];
        }
        AutoLease lease{client};
        if (!EnsureLease(client, lease)) return 2;
        std::vector<std::string> output;
        if (!client.sendCommand(line, &output, &error, 200)) {
            std::cerr << "rtectl: " << error << '\n';
            return 1;
        }
        for (const auto& outputLine : output) std::cout << outputLine << '\n';
        return 0;
    }
    if (command == "flash") {
        if (index >= argc) { Usage(); return 1; }
        const std::string path = argv[index];
        AutoLease lease{client};
        if (!EnsureLease(client, lease)) return 2;
        std::string job;
        if (!client.flashFile(path, true, &job, &error)) {
            std::cerr << "rtectl: " << error << '\n';
            return 1;
        }
        std::size_t shown = 0;
        for (;;) {
            const auto status = client.flashStatus(job);
            if (!status.reachable) {
                std::cerr << "rtectl: " << status.lastError << '\n';
                return 1;
            }
            while (shown < status.log.size()) std::cout << status.log[shown++] << '\n';
            if (!status.busy) {
                if (status.state == "Done") return 0;
                std::cerr << "rtectl: " << status.lastError << '\n';
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    if (command == "control") {
        if (index >= argc) { Usage(); return 1; }
        const std::string action = argv[index];
        if (action == "status") {
            const std::string body = client.stateJson(&error);
            if (body.empty()) {
                std::cerr << "rtectl: " << error << '\n';
                return 1;
            }
            PrettyPrint(body);
            return 0;
        }
        if (action == "release") {
            if (!client.hasLease()) {
                std::cerr << "rtectl: --lease ID is required\n";
                return 1;
            }
            if (!client.releaseLease(&error)) {
                std::cerr << "rtectl: " << error << '\n';
                return 1;
            }
            return 0;
        }
        if (action == "hold") {
            std::signal(SIGINT, OnSignal);
            std::signal(SIGTERM, OnSignal);
            if (!client.hasLease() && !client.acquireLease("rtectl", &error)) {
                std::cerr << "rtectl: " << error << '\n';
                return 2;
            }
            std::cout << client.leaseId() << '\n';
            client.startEvents();
            while (!gStop && client.hasLease()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            client.stopEvents();
            client.releaseLease();
            return 0;
        }
    }

    Usage();
    return 1;
}
