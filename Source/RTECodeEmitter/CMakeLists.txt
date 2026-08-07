add_executable(RTECodeEmitter src/Main.cpp)

target_link_libraries(RTECodeEmitter
    PRIVATE RTEAutomation project_warnings
)

# Static-link the MinGW runtime so the emitter starts from any launch context
# (e.g. NodeGUI's "Build Simulation" spawns it via PowerShell without Qt's
# mingw bin on PATH — missing libstdc++/libgcc DLLs fail silently there).
if(MINGW)
    target_link_options(RTECodeEmitter PRIVATE -static)
endif()

add_executable(RTECodeEmitter_tests
    tests/test_marker_parser.cpp
    tests/test_emitter.cpp
)

target_link_libraries(RTECodeEmitter_tests
    PRIVATE RTEAutomation GTest::gtest_main project_warnings
)

gtest_discover_tests(RTECodeEmitter_tests)
