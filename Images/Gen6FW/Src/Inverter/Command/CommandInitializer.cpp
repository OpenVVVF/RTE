#include "Inverter/Command/CommandInitializer.h"
#include "Inverter/Command/CommandManager.h"

void registerOpenLoopCommands(CommandManager& mgr);
void registerSystemCommands(CommandManager& mgr);
void registerCalibrationCommands(CommandManager& mgr);
void registerFaultCommands(CommandManager& mgr);
void registerSensorCommands(CommandManager& mgr);
void registerHelpCommand(CommandManager& mgr);
void registerFocCommands(CommandManager& mgr);
void registerMotorConfigCommands(CommandManager& mgr);

/* TIME_DOMAIN: APPLICATION_COMMAND_REGISTRATION
 *   Registers all shell commands at boot.  Commands execute in main-loop context.
 * CODEGEN: Add generated command tables here (motor start/stop, tuning,
 *   calibration, CAN configuration, application-specific commands, etc.).
 */
void initializeCommands() {
    CommandManager& mgr = CommandManager::instance();

    registerOpenLoopCommands(mgr);
    registerSystemCommands(mgr);
    registerCalibrationCommands(mgr);
    registerFaultCommands(mgr);
    registerSensorCommands(mgr);
    registerFocCommands(mgr);
    registerMotorConfigCommands(mgr);
    registerHelpCommand(mgr);
}
