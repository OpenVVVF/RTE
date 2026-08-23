add_executable(RTEFirmwareBuilder src/Main.cpp)

target_link_libraries(RTEFirmwareBuilder
    PRIVATE RTEAutomation project_warnings
)

# Ensure RTECodeEmitter is built so the builder can invoke it for --graph flows.
add_dependencies(RTEFirmwareBuilder RTECodeEmitter)
