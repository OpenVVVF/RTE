# Shared gateway runtime and client components. The gateway owns and parses
# serial once; NodeGUI, rtectl, and future adapters consume the versioned API.

add_library(RTEShared STATIC
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

set_target_properties(RTEShared PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(RTEShared
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
)

target_compile_definitions(RTEShared
    PRIVATE RTE_TOOLS_DIR="${CMAKE_SOURCE_DIR}/Tools"
)

target_link_libraries(RTEShared
    PUBLIC InverterProtocol nlohmann_json
    PRIVATE project_warnings
)

add_executable(RTEShared_tests tests/test_gateway.cpp)
target_link_libraries(RTEShared_tests
    PRIVATE RTEShared GTest::gtest_main project_warnings
)
include(GoogleTest)
gtest_discover_tests(RTEShared_tests)
