cmake_minimum_required(VERSION 3.24)
project(NodeAPI VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(NODEAPI_BUILD_TIMING "Build the optional timing-domain validation extension" ON)

# Shared warning flags.
add_library(project_warnings INTERFACE)
if(MSVC)
    target_compile_options(project_warnings INTERFACE /W4)
else()
    # -Wno-missing-field-initializers: the public structs intentionally have many
    # optional fields; designated initializers are the primary construction style.
    target_compile_options(project_warnings INTERFACE -Wall -Wextra -Wno-missing-field-initializers)
endif()

enable_testing()
add_subdirectory(third_party)
include(GoogleTest)

set(NODEAPI_SOURCES
    src/Graph.cpp
    src/NodeTemplates.cpp
    src/NodeType.cpp
    src/Serialization.cpp
    src/WireType.cpp
)

if(NODEAPI_BUILD_TIMING)
    list(APPEND NODEAPI_SOURCES src/Timing.cpp)
endif()

add_library(NodeAPI STATIC ${NODEAPI_SOURCES})

target_include_directories(NodeAPI
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(NodeAPI
    PUBLIC nlohmann_json
    PRIVATE project_warnings
)

set(NODEAPI_TEST_SOURCES
    tests/test_nodeapi.cpp
    tests/test_templates.cpp
)

if(NODEAPI_BUILD_TIMING)
    list(APPEND NODEAPI_TEST_SOURCES tests/test_timing.cpp)
endif()

add_executable(NodeAPI_tests ${NODEAPI_TEST_SOURCES})

target_link_libraries(NodeAPI_tests
    PRIVATE NodeAPI GTest::gtest_main project_warnings
)

if(NODEAPI_BUILD_TIMING)
    target_compile_definitions(NodeAPI_tests PRIVATE NODEAPI_BUILD_TIMING=1)
endif()

gtest_discover_tests(NodeAPI_tests)
