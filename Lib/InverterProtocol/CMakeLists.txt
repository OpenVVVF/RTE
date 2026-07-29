cmake_minimum_required(VERSION 3.24)
project(InverterProtocol VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(INVERTERPROTOCOL_BUILD_TESTS "Build InverterProtocol tests" ON)

# Warning flags for this library.
add_library(ivp_warnings INTERFACE)
if(MSVC)
    target_compile_options(ivp_warnings INTERFACE /W4)
else()
    target_compile_options(ivp_warnings INTERFACE -Wall -Wextra -Wno-missing-field-initializers)
endif()

# ========================================================================
# Portable C core (host + device compatible)
# ========================================================================
set(INVERTERPROTOCOL_CORE_SOURCES
    src/protocol.c
    src/packet_builder.c
    src/packet_parser.c
)

add_library(InverterProtocolCore STATIC ${INVERTERPROTOCOL_CORE_SOURCES})

target_include_directories(InverterProtocolCore
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(InverterProtocolCore
    PRIVATE ivp_warnings
)

# ========================================================================
# Host C++ layer (transport + client)
# ========================================================================
set(INVERTERPROTOCOL_HOST_SOURCES
    src/host/uart_transport.cpp
    src/host/tcp_transport.cpp
    src/host/host_client.cpp
)

add_library(InverterProtocol STATIC ${INVERTERPROTOCOL_HOST_SOURCES})

target_include_directories(InverterProtocol
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(InverterProtocol
    PUBLIC InverterProtocolCore
    PRIVATE ivp_warnings
)

if(WIN32)
    target_link_libraries(InverterProtocol PRIVATE ws2_32)
endif()

# ========================================================================
# Tests
# ========================================================================
if(INVERTERPROTOCOL_BUILD_TESTS)
    include(GoogleTest)

    add_executable(InverterProtocol_tests
        tests/test_protocol.cpp
    )

    target_link_libraries(InverterProtocol_tests
        PRIVATE InverterProtocol InverterProtocolCore GTest::gtest_main ivp_warnings
    )
    if(UNIX)
        target_link_libraries(InverterProtocol_tests PRIVATE util)
    endif()

    gtest_discover_tests(InverterProtocol_tests)
endif()
