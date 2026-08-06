cmake_minimum_required(VERSION 3.24)
project(RTE VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Keep user-facing binaries and libraries out of CMake's source-directory
# mirror (build/Source/...).  Multi-config generators retain a configuration
# component so Debug and Release products cannot overwrite one another.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
if(CMAKE_CONFIGURATION_TYPES)
    foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${config}" config_upper)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin/${config}")
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/lib/${config}")
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/lib/${config}")
    endforeach()
endif()

enable_testing()

add_subdirectory(Lib/NodeAPI)
add_subdirectory(Lib/RTELogger)
add_subdirectory(Lib/InverterCodegen)
add_subdirectory(Lib/InverterProtocol)
add_subdirectory(Source/RTEAutomation)
add_subdirectory(Source/RTECodeEmitter)
add_subdirectory(Source/RTEFirmwareBuilder)
add_subdirectory(Source/RTECLI)
add_subdirectory(Source/NodeGUI)

install(TARGETS rte RTECodeEmitter RTEFirmwareBuilder
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY Assets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/rte/Assets)
install(DIRECTORY Images/Gen6FW/ DESTINATION ${CMAKE_INSTALL_DATADIR}/rte/Images/Gen6FW
    PATTERN "build" EXCLUDE
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
    PATTERN ".vscode" EXCLUDE
)
install(FILES Tools/mcp2221a_gpio.py
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rte/tools
)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/rte)

set(CPACK_PACKAGE_NAME "RTE")
set(CPACK_PACKAGE_VENDOR "OpenVVVF")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME
    "rte-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
if(WIN32)
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR TGZ)
endif()
include(CPack)
