add_executable(RTECodeEmitter
    src/Main.cpp
    src/Emitter.cpp
    src/MarkerParser.cpp
)

target_link_libraries(RTECodeEmitter
    PRIVATE InverterCodegenLib NodeAPI RTELogger project_warnings
)

add_executable(RTECodeEmitter_tests
    tests/test_marker_parser.cpp
    tests/test_emitter.cpp
    src/Emitter.cpp
    src/MarkerParser.cpp
)

target_link_libraries(RTECodeEmitter_tests
    PRIVATE InverterCodegenLib NodeAPI RTELogger GTest::gtest_main project_warnings
)

gtest_discover_tests(RTECodeEmitter_tests)
