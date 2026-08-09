#include "Inverter/Command/CommandInitializer.h"
#include "Inverter/Command/CommandManager.h"

void registerSystemCommands(CommandManager& mgr);
void registerFaultCommands(CommandManager& mgr);
void registerSensorCommands(CommandManager& mgr);
void registerHelpCommand(CommandManager& mgr);
void registerControlCommands(CommandManager& mgr);
void registerCalibrationCommands(CommandManager& mgr);
void registerFocCommands(CommandManager& mgr);
void registerCanCommands(CommandManager& mgr);
void registerOpenLoopCommands(CommandManager& mgr);
void registerTraceCommands(CommandManager& mgr);

/* TIME_DOMAIN: APPLICATION_COMMAND_REGISTRATION
 *   Registers all shell commands at boot.  Commands execute in main-loop context.
 * CODEGEN: Add generated command tables here (motor start/stop, tuning,
 *   calibration, CAN configuration, application-specific commands, etc.).
 */
void initializeCommands() {
    CommandManager& mgr = CommandManager::instance();

    registerSystemCommands(mgr);
    registerFaultCommands(mgr);
    registerSensorCommands(mgr);
    registerHelpCommand(mgr);
    registerControlCommands(mgr);
    registerCalibrationCommands(mgr);
    registerFocCommands(mgr);
    registerCanCommands(mgr);
    registerOpenLoopCommands(mgr);
    registerTraceCommands(mgr);
}
