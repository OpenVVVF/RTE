#pragma once
namespace Inverter {
class OpenLoopController {
public:
    bool isRunning() const { return running; }
    void stop() { running = false; }
    bool running = false;
};
OpenLoopController& openLoopController();
}
