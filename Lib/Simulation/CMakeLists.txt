cmake_minimum_required(VERSION 3.24)
project(Simulation VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(Simulation INTERFACE)
target_include_directories(Simulation INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(TARGET project_warnings)
    target_link_libraries(Simulation INTERFACE project_warnings)
endif()

enable_testing()
include(GoogleTest)

function(add_sim_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Simulation GTest::gtest_main)
    if(TARGET project_warnings)
        target_link_libraries(${name} PRIVATE project_warnings)
    endif()
    gtest_discover_tests(${name})
endfunction()

add_sim_test(Simulation_inverter_tests tests/test_inverter.cpp)
add_sim_test(Simulation_pmsm_tests tests/test_pmsm.cpp)
add_sim_test(Simulation_mpcc_tests tests/test_mpcc.cpp)

add_executable(mpcc_closed_loop simulation/mpcc_closed_loop.cpp)
target_link_libraries(mpcc_closed_loop PRIVATE Simulation)
if(TARGET project_warnings)
    target_link_libraries(mpcc_closed_loop PRIVATE project_warnings)
endif()

add_executable(compare_mpcc_foc simulation/compare_mpcc_foc.cpp)
target_link_libraries(compare_mpcc_foc PRIVATE Simulation)
if(TARGET project_warnings)
    target_link_libraries(compare_mpcc_foc PRIVATE project_warnings)
endif()
