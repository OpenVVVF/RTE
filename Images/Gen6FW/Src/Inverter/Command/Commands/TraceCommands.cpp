#include "Inverter/Command/CommandContext.h"
#include "Inverter/Command/CommandInterface.h"
#include "Inverter/Command/CommandManager.h"
#include "Inverter/Drivers/Logging/TraceRecorder.h"
#include "Inverter/Telemetry.h"

#include <strings.h>

class TraceCommand : public CommandInterface {
public:
    TraceCommand() : CommandInterface("trace", "Supplemental CAN-FD trace: status/start/stop",
        ArgSpec{"subcommand", "", 0, 0, 0, true, ArgSpec::STRING}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        if (strcasecmp(args[0].s_val, "status") == 0) {
            Inverter::traceRecorder().printStatus();
        } else if (strcasecmp(args[0].s_val, "start") == 0) {
            Inverter::traceRecorder().start();
            Inverter::traceRecorder().printStatus();
        } else if (strcasecmp(args[0].s_val, "stop") == 0) {
            Inverter::traceRecorder().stop();
            Inverter::traceRecorder().printStatus();
        } else {
            Telemetry::printf("[SHELL] usage: trace status|start|stop");
        }
    }
};

static TraceCommand s_trace_command;

void registerTraceCommands(CommandManager& manager) {
    manager.registerCommand(&s_trace_command);
}
