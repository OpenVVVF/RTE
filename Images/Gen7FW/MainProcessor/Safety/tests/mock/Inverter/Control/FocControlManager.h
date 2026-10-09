#pragma once
namespace Inverter {
class FocControlManager {
public:
    bool isRunning() const { return running; }
    void stop() { running = false; }
    bool running = false;
};
FocControlManager& focControlManager();
}
