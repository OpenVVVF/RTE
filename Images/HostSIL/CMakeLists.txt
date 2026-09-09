cmake_minimum_required(VERSION 3.24)
project(HostSIL LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

# ---------------------------------------------------------------------------
# Layout
#
#   HostSIL/          this tree (SIL runtime, shims, scenario)
#   ../Gen6FW/        firmware base image (read-only, compiled verbatim)
#   ../HostSim/src    ODE PMSM plant (compiled from its original location)
#   <build>/hostsil_fw_src   firmware copy patched+emitted by RTECodeEmitter
#
# Nothing under Images/Gen6FW or Images/HostSim is ever written.
# ---------------------------------------------------------------------------
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(GEN6FW_ROOT "${REPO_ROOT}/Images/Gen6FW" CACHE PATH "Firmware base image root")
set(HOSTSIM_ROOT "${REPO_ROOT}/Images/HostSim" CACHE PATH "HostSim root")
set(IVP_CORE_DIR "${REPO_ROOT}/Lib/InverterProtocol" CACHE PATH "InverterProtocol core root")
set(SIL_GRAPH "${REPO_ROOT}/Assets/Examples/foc_demo.json" CACHE FILEPATH
    "Node graph used to emit the firmware control code")

set(SIL_FW_SRC "${REPO_ROOT}/build/hostsil_fw_src" CACHE PATH
    "Emitted firmware tree (Gen6FW copy with generated/ domain code)")

if(NOT IS_DIRECTORY "${GEN6FW_ROOT}/Src/Inverter")
    message(FATAL_ERROR "Gen6FW not found at ${GEN6FW_ROOT}")
endif()
if(NOT IS_DIRECTORY "${IVP_CORE_DIR}/include")
    message(FATAL_ERROR "InverterProtocol not found at ${IVP_CORE_DIR}")
endif()

# ---------------------------------------------------------------------------
# Emit the firmware tree (copy of Gen6FW + generated domain code).
# Same emitter invocation as Images/HostSim/scripts/emit_and_run.sh uses,
# with Gen6FW as the base image.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${SIL_FW_SRC}/Src/Inverter/InverterMain.cpp")
    find_program(RTE_EMITTER
        NAMES RTECodeEmitter rte
        PATHS "${REPO_ROOT}/build/bin"
        NO_DEFAULT_PATH)
    if(NOT RTE_EMITTER)
        message(FATAL_ERROR
            "RTECodeEmitter not found. Build host tools first:\n"
            "  cmake -S ${REPO_ROOT} -B ${REPO_ROOT}/build && "
            "cmake --build ${REPO_ROOT}/build --target RTECodeEmitter\n"
            "or emit manually: Images/HostSIL/scripts/emit_firmware.sh")
    endif()
    get_filename_component(_emit_name "${RTE_EMITTER}" NAME)
    if(_emit_name STREQUAL "rte")
        set(_emit_args generate --graph "${SIL_GRAPH}"
            --base-source "${GEN6FW_ROOT}" --output "${SIL_FW_SRC}")
    else()
        set(_emit_args --base-src "${GEN6FW_ROOT}" --graph "${SIL_GRAPH}"
            --output "${SIL_FW_SRC}")
    endif()
    message(STATUS "Emitting firmware: ${RTE_EMITTER} ${_emit_args}")
    execute_process(
        COMMAND "${RTE_EMITTER}" ${_emit_args}
        RESULT_VARIABLE _emit_rc)
    if(NOT _emit_rc EQUAL 0)
        message(FATAL_ERROR "RTECodeEmitter failed (${_emit_rc})")
    endif()
endif()

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS
    "${SIL_FW_SRC}/generated/*_generated.cpp"
)
if(NOT GENERATED_SOURCES)
    message(FATAL_ERROR "no generated domain sources under ${SIL_FW_SRC}/generated")
endif()

set(SIL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sil")

add_executable(host_sil
    # --- SIL runtime (host side) ---
    src/main.cpp
    src/scenario.cpp
    sil/sil_rt.cpp
    sil/sil_world.cpp
    sil/sil_hal.cpp
    # --- SIL driver shims (same headers/classes as the hardware drivers) ---
    sil/sil_pwm.cpp
    sil/sil_phase_current_adc.cpp
    sil/sil_encoder_adc.cpp
    sil/sil_max22530.cpp
    sil/sil_app_sensors.cpp
    sil/sil_can.cpp
    sil/sil_misc.cpp
    sil/sil_fram.c
    sil/sil_uart_c.c
    sil/sil_uart_cpp.cpp
    sil/sil_live_server.cpp
    # --- HostSim plant ---
    "${HOSTSIM_ROOT}/src/motor_model.cpp"
    # --- Gen6FW application code, verbatim ---
    "${SIL_FW_SRC}/Src/Inverter/InverterMain.cpp"
    "${SIL_FW_SRC}/Src/Inverter/LoopStats.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Telemetry.cpp"
    "${SIL_FW_SRC}/Src/Inverter/platform_api.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Control/CommandShell.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Control/ControlSupervisor.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Control/CurrentObserver.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Control/FaultCallbacks.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Control/FaultManager.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Control/FocController.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Control/FocControlManager.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Control/MotorParameterEstimator.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Control/OpenLoopController.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Control/VectorPIController.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Control/VoltageVectorSchedule.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/AutoCalibrationCoordinator.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/BreakawayCalibrator.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/CalKvStore.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/EncoderCycleCalibrator.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/EncoderLinearityCalibrator.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/EncoderOffsetCalibrator.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/FluxLinkageCalibrator.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/InductanceCalibrator.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/InductionMotorCalibrator.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/InductionVHzCalibrator.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/MotorCalibration.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/PoleCalibrator.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/ResistanceCalibrator.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/Common/BreakawayFinder.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/Common/CalibrationHardware.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/Common/CalScratchBuffer.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/Common/CurrentLimitedRamp.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Calibration/Common/EncoderTracker.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Command/CommandInitializer.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Command/CommandManager.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Command/Commands/CalibrationCommands.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Command/Commands/CanCommands.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Command/Commands/ControlCommands.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Command/Commands/FaultCommands.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Command/Commands/FocCommands.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Command/Commands/HelpCommand.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Command/Commands/MotorConfigCommands.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Command/Commands/OpenLoopCommands.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Command/Commands/SensorCommands.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Command/Commands/SystemCommands.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Command/Commands/TraceCommands.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Drivers/Sensors/CurrentSensor.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Drivers/Sensors/CurrentSensorTest.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Drivers/Sensors/DcLinkCurrentSensor.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Drivers/Sensors/DcLinkVoltageSensor.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Drivers/Sensors/PoleEstimator.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Drivers/Sensors/SpikeRecorder.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Drivers/CAN/CanProtocolTransport.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Drivers/CAN/CanSession.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Drivers/Logging/TraceRecorder.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Drivers/Logging/ontime_logger.c"
    "${SIL_FW_SRC}/Src/Inverter/Drivers/Storage/FramStore.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Drivers/Storage/MotorConfigStore.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Drivers/Storage/RteParamStore.cpp"
    "${SIL_FW_SRC}/Src/Inverter/Drivers/GateDriver/gate_driver.c"
    # --- RTE-generated control domains ---
    ${GENERATED_SOURCES}
    # --- Shared InverterProtocol C core (telemetry/session wire format) ---
    "${IVP_CORE_DIR}/src/protocol.c"
    "${IVP_CORE_DIR}/src/packet_builder.c"
    "${IVP_CORE_DIR}/src/packet_parser.c"
    "${IVP_CORE_DIR}/src/trace_protocol.c"
)

# Include-path ORDER MATTERS: sil/stm32shim shadows the CubeMX/HAL headers
# (main.h, adc.h, tim.h, ...) before the firmware's own Inc tree.
target_include_directories(host_sil PRIVATE
    ${SIL_DIR}/stm32shim
    ${SIL_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SIL_FW_SRC}/generated
    ${SIL_FW_SRC}/Inc
    ${SIL_FW_SRC}/Inc/Inverter
    ${SIL_FW_SRC}/Inc/Inverter/Control
    ${SIL_FW_SRC}/Inc/Inverter/Control/Math
    ${SIL_FW_SRC}/Inc/Inverter/Calibration
    ${SIL_FW_SRC}/Inc/Inverter/Calibration/Common
    ${SIL_FW_SRC}/Inc/Inverter/Command
    ${SIL_FW_SRC}/Inc/Inverter/Drivers/GateDriver
    ${SIL_FW_SRC}/Inc/Inverter/Drivers/CAN
    ${SIL_FW_SRC}/Inc/Inverter/Drivers/UART
    ${SIL_FW_SRC}/Inc/Inverter/Drivers/Storage
    ${SIL_FW_SRC}/Inc/Inverter/Drivers/Logging
    ${SIL_FW_SRC}/Inc/Inverter/Drivers/PWM
    ${SIL_FW_SRC}/Inc/Inverter/Drivers/Sensors
    ${HOSTSIM_ROOT}/src
    "${IVP_CORE_DIR}/include"
)

if(UNIX)
    target_link_libraries(host_sil PRIVATE m pthread)
endif()

if(MSVC)
    target_compile_options(host_sil PRIVATE /W4 /wd4200)
else()
    target_compile_options(host_sil PRIVATE -Wall -Wextra
        # Volatile-bitfield aliasing and missing-field initializers fire
        # inside the untouched firmware TUs; keep the signal for HostSIL files.
        -Wno-unused-parameter
        # A few upstream firmware TUs use std:: math/cstring without the
        # includes (their ARM toolchain pulls them in transitively).
        $<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cmath -include cstring>)
endif()
