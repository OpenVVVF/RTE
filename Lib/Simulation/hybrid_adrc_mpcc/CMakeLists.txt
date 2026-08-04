cmake_minimum_required(VERSION 3.24)
project(hybrid_adrc_mpcc LANGUAGES CXX)

# Fully standalone package — does not modify parent Simulation/CMakeLists.txt.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SIM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(REPO_RESULTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../results/adrc_mpcc_hybrid")

add_library(hybrid_adrc_mpcc_headers INTERFACE)
target_include_directories(hybrid_adrc_mpcc_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${SIM_ROOT}/include
)

add_executable(adrc_mpcc_hybrid_sim adrc_mpcc_hybrid_sim.cpp)
target_link_libraries(adrc_mpcc_hybrid_sim PRIVATE hybrid_adrc_mpcc_headers)

enable_testing()
find_package(GTest QUIET)
if(GTest_FOUND)
    add_executable(test_adrc_mpcc_hybrid test_adrc_mpcc_hybrid.cpp)
    target_link_libraries(test_adrc_mpcc_hybrid PRIVATE hybrid_adrc_mpcc_headers GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(test_adrc_mpcc_hybrid)
else()
    # Fallback: compile a tiny no-gtest smoke binary is skipped; use run_hybrid.sh --test with system gtest if present.
    message(STATUS "GTest not found — unit test target skipped. Sim binary still builds.")
endif()
