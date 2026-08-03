# Shared host-side runtime components: telemetry client (legacy wire
# protocol, serial or TCP), telemetry data model, flash worker, and the
# loopback/LAN HTTP API. Used by both RTEServer and NodeGUI.

add_library(RTEShared STATIC
    src/LegacyTelemetryClient.cpp
    src/LegacyTelemetryClient.h
    src/TelemetryStore.cpp
    src/TelemetryStore.h
    src/FirmwareUpdater.cpp
    src/FirmwareUpdater.h
    src/HttpApiServer.cpp
    src/HttpApiServer.h
)

set_target_properties(RTEShared PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(RTEShared
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(RTEShared
    PUBLIC InverterProtocol
    PRIVATE project_warnings
)
