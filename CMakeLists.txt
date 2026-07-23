cmake_minimum_required(VERSION 3.24)
project(RTE VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_subdirectory(Lib/NodeAPI)
add_subdirectory(Lib/InverterCodegen)
add_subdirectory(Source/RTECodeEmitter)
