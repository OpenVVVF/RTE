cmake_minimum_required(VERSION 3.24)
project(HostSim LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    ${GENERATED_SOURCES}
)

target_include_directories(host_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(UNIX)
    target_link_libraries(host_sim PRIVATE m)
endif()

if(MSVC)
    target_compile_options(host_sim PRIVATE /W4)
else()
    target_compile_options(host_sim PRIVATE -Wall -Wextra -Wpedantic)
endif()
