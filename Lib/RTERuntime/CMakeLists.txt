# Reusable, Qt-free runtime and API client code. The gateway owns and parses
# serial once; NodeGUI, rtectl, and future adapters consume the versioned API.

add_library(RTERuntime STATIC
    src/LegacyTelemetryClient.cpp
    src/LegacyTelemetryClient.h
    src/TelemetryStore.cpp
    src/TelemetryStore.h
    src/FirmwareUpdater.cpp
    src/FirmwareUpdater.h
    src/GatewayApiServer.cpp
    src/GatewayApiServer.h
    src/GatewayClient.cpp
    src/GatewayClient.h
    src/Sha256.cpp
    src/Sha256.h
)

add_library(RTE::Runtime ALIAS RTERuntime)

set_target_properties(RTERuntime PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(RTERuntime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
)

target_compile_definitions(RTERuntime
    PRIVATE RTE_TOOLS_DIR="${CMAKE_SOURCE_DIR}/Tools"
)

target_link_libraries(RTERuntime
    PUBLIC InverterProtocol nlohmann_json
    PRIVATE project_warnings
)

add_executable(RTERuntime_tests tests/test_gateway.cpp)
target_link_libraries(RTERuntime_tests
    PRIVATE RTE::Runtime GTest::gtest_main project_warnings
)
include(GoogleTest)
gtest_discover_tests(RTERuntime_tests)
