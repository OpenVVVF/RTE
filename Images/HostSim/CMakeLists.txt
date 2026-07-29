cmake_minimum_required(VERSION 3.24)
project(HostSim LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(IVP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../Lib/InverterProtocol")
if(NOT EXISTS "${IVP_ROOT}/include/inverter_protocol/protocol.h")
    message(FATAL_ERROR "InverterProtocol not found at ${IVP_ROOT}")
endif()

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/domain_*_generated.cpp"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/bridges_generated.cpp")
    list(APPEND GENERATED_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/generated/bridges_generated.cpp")
endif()

add_executable(host_sim
    src/main.cpp
    src/platform_api.cpp
    src/sim_runtime.cpp
    src/motor_model.cpp
    src/pwm_scope.cpp
    src/telemetry_publisher.cpp
    src/realtime_platform.cpp
    # Portable IVP C core (no host serial layer needed for the publisher).
    "${IVP_ROOT}/src/protocol.c"
    "${IVP_ROOT}/src/packet_builder.c"
    "${IVP_ROOT}/src/packet_parser.c"
    ${GENERATED_SOURCES}
)

target_include_directories(host_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    "${IVP_ROOT}/include"
)

if(WIN32)
    target_link_libraries(host_sim PRIVATE ws2_32 winmm avrt)
endif()

if(UNIX)
    target_link_libraries(host_sim PRIVATE m)
endif()

if(MSVC)
    target_compile_options(host_sim PRIVATE /W4 /wd4200)
else()
    target_compile_options(host_sim PRIVATE -Wall -Wextra -Wpedantic)
endif()
