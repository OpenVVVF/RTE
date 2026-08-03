#pragma once

#include <string>
#include <vector>

namespace NodeGUI::runtime {

// Status snapshot shown in the Firmware Update tab.
struct FlashBackendStatus {
    std::string state = "Idle";
    bool busy = false;
    std::string lastError;
    std::vector<std::string> log;
    int progress = -1;     // 0..100, -1 = indeterminate
    bool reachable = false;  // server answered the last status poll
};

// Where the Firmware Update tab sends flash jobs. The GUI ALWAYS talks to an
// RTEServer over HTTP (spawned locally or remote) — there is deliberately no
// local-serial backend anymore, so all flashing goes through one code path.
class FlashBackend {
public:
    virtual ~FlashBackend() = default;

    // Queue a flash of the given firmware file. Returns false when the server
    // rejects the request (busy / unreachable / bad file).
    virtual bool QueueFlash(const std::string& firmwarePath, bool autoGpio) = 0;

    virtual FlashBackendStatus Status() = 0;
};

}  // namespace NodeGUI::runtime
