add_executable(RTEFirmwareBuilder
    src/Main.cpp
    src/Builder.cpp
    src/Toolchain.cpp
)

target_link_libraries(RTEFirmwareBuilder
    PRIVATE RTELogger project_warnings
)

# Ensure RTECodeEmitter is built so the builder can invoke it for --graph flows.
add_dependencies(RTEFirmwareBuilder RTECodeEmitter)
