add_executable(RTECodeEmitter src/Main.cpp)

target_link_libraries(RTECodeEmitter
    PRIVATE RTEAutomation project_warnings
)

add_executable(RTECodeEmitter_tests
    tests/test_marker_parser.cpp
    tests/test_emitter.cpp
)

target_link_libraries(RTECodeEmitter_tests
    PRIVATE RTEAutomation GTest::gtest_main project_warnings
)

gtest_discover_tests(RTECodeEmitter_tests)
