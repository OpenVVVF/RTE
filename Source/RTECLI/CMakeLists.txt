add_executable(rte src/Main.cpp)
target_link_libraries(rte PRIVATE RTEAutomation project_warnings)

set_target_properties(rte PROPERTIES OUTPUT_NAME rte)

add_executable(RTECLI_tests tests/test_cli.cpp)
target_compile_definitions(RTECLI_tests PRIVATE RTE_CLI_PATH="$<TARGET_FILE:rte>")
target_link_libraries(RTECLI_tests PRIVATE RTEAutomation GTest::gtest_main project_warnings)
add_dependencies(RTECLI_tests rte)
gtest_discover_tests(RTECLI_tests)
