add_executable(rte src/Main.cpp src/TraceCli.cpp)
target_link_libraries(rte PRIVATE RTEAutomation InverterProtocolCore project_warnings)

set_target_properties(rte PROPERTIES OUTPUT_NAME rte)

add_executable(RTECLI_tests tests/test_cli.cpp)
target_compile_definitions(RTECLI_tests PRIVATE RTE_CLI_PATH="$<TARGET_FILE:rte>")
target_link_libraries(RTECLI_tests PRIVATE RTEAutomation InverterProtocolCore GTest::gtest_main project_warnings)
add_dependencies(RTECLI_tests rte)
gtest_discover_tests(RTECLI_tests)

if(UNIX)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME RTECLI_mcp_flash_integration
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mcp_flash.py
                    $<TARGET_FILE:rte> ${CMAKE_SOURCE_DIR})
    endif()
endif()
