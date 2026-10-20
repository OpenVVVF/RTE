add_executable(rte src/Main.cpp src/SimCli.cpp src/TraceCli.cpp)
target_link_libraries(rte PRIVATE RTEAutomation RTESimLib InverterProtocolCore project_warnings)

set_target_properties(rte PROPERTIES OUTPUT_NAME rte)

add_executable(RTECLI_tests tests/test_cli.cpp)
target_compile_definitions(RTECLI_tests PRIVATE RTE_CLI_PATH="$<TARGET_FILE:rte>")
target_link_libraries(RTECLI_tests PRIVATE RTEAutomation InverterProtocolCore GTest::gtest_main project_warnings)
add_dependencies(RTECLI_tests rte)
# GoogleTest was only include()d inside Lib/NodeAPI's directory scope; without
# re-including here, gtest_discover_tests emits an empty discovery script path
# (-P "") and ctest reports RTECLI_tests_NOT_BUILT.
include(GoogleTest)
gtest_discover_tests(RTECLI_tests)
