#pragma once
namespace Inverter {
class ControlSupervisor {
public:
    static ControlSupervisor& instance();
    void notifySafetyShutdown() { faulted = true; }
    bool faulted = false;
};
}
