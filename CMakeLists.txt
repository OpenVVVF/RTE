cmake_minimum_required(VERSION 3.24)
project(RTE VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(FetchContent)

# Linux exposes MCP2221A pins through the kernel GPIO character-device API.
# Windows and macOS use HIDAPI directly. Pin the HIDAPI source archive and
# checksum so a build cannot silently pick up a different implementation.
option(RTE_ENABLE_MCP2221_GPIO "Build native MCP2221A GPIO support" ON)
if(RTE_ENABLE_MCP2221_GPIO AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(HIDAPI_WITH_LIBUSB OFF CACHE BOOL "" FORCE)
    set(HIDAPI_WITH_HIDRAW ON CACHE BOOL "" FORCE)
    set(HIDAPI_BUILD_HIDTEST OFF CACHE BOOL "" FORCE)
    set(HIDAPI_WITH_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(hidapi
        URL https://github.com/libusb/hidapi/archive/refs/tags/hidapi-0.15.0.tar.gz
        URL_HASH SHA256=5d84dec684c27b97b921d2f3b73218cb773cf4ea915caee317ac8fc73cef8136
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(hidapi)
    install(FILES ${hidapi_SOURCE_DIR}/LICENSE-bsd.txt
        DESTINATION ${CMAKE_INSTALL_DATADIR}/rte/licenses
        RENAME hidapi-BSD.txt
    )
endif()

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
option(BUILD_NODEGUI "Build the NodeGUI Qt application" ON)
if(BUILD_NODEGUI)
    add_subdirectory(Source/NodeGUI)
endif()

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
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/rte)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(FILES packaging/udev/60-rte-mcp2221.rules
        DESTINATION ${CMAKE_INSTALL_DATADIR}/rte/udev
    )
endif()

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
=======
option(BUILD_NODEGUI "Build the NodeGUI Qt application" ON)
if(BUILD_NODEGUI)
    add_subdirectory(Source/NodeGUI)
endif()
>>>>>>> 355ae6c (Fix SVPWM resolution, WSL permissions, and NodeGUI multi-threading defects)
