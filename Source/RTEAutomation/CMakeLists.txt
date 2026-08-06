add_library(RTEAutomation STATIC
    ../RTECodeEmitter/src/Emitter.cpp
    ../RTECodeEmitter/src/MarkerParser.cpp
    ../RTEFirmwareBuilder/src/Builder.cpp
    ../RTEFirmwareBuilder/src/Toolchain.cpp
    src/CachePaths.cpp
    src/Flasher.cpp
    src/Platform.cpp
    src/ProcessRunner.cpp
    src/Session.cpp
)

target_include_directories(RTEAutomation
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../RTECodeEmitter/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../RTEFirmwareBuilder/src
)

target_compile_definitions(RTEAutomation
    PRIVATE
        RTE_INVERTERCODEGEN_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/Lib/InverterCodegen/include"
        RTE_INVERTERCODEGEN_THIRD_PARTY_DIR="${CMAKE_SOURCE_DIR}/Lib/InverterCodegen/third_party"
)

target_link_libraries(RTEAutomation
    PUBLIC
        InverterCodegenLib
        NodeAPI
        RTELogger
        InverterProtocol
        nlohmann_json
    PRIVATE project_warnings
)

if(WIN32)
    target_link_libraries(RTEAutomation PRIVATE ws2_32 shell32 ole32)
endif()

add_executable(RTEAutomation_tests
    tests/test_cache_paths.cpp
    tests/test_process_runner.cpp
)
target_link_libraries(RTEAutomation_tests
    PRIVATE RTEAutomation GTest::gtest_main project_warnings
)
gtest_discover_tests(RTEAutomation_tests)
