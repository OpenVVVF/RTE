cmake_minimum_required(VERSION 3.24)
project(Simulation VERSION 0.1.0 LANGUAGES CXX)

# Header-only plant/controller helpers used by hybrid_adrc_mpcc (standalone
# package under hybrid_adrc_mpcc/ with its own CMake via run_hybrid.sh).
add_library(Simulation INTERFACE)
target_include_directories(Simulation INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
