cmake_minimum_required(VERSION 3.24)
project(RTE VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_subdirectory(Lib/NodeAPI)
add_subdirectory(Lib/RTELogger)
add_subdirectory(Lib/InverterCodegen)
add_subdirectory(Lib/InverterProtocol)
add_subdirectory(Lib/RTERuntime)
add_subdirectory(Source/RTEGateway)
add_subdirectory(Source/RTECtl)
add_subdirectory(Source/RTECodeEmitter)
add_subdirectory(Source/RTEFirmwareBuilder)
add_subdirectory(Source/NodeGUI)

install(PROGRAMS Tools/mcp2221a_gpio.py
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/rte)
install(FILES Deploy/rte-gateway.json Deploy/rte-gateway.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/rte/deploy)
