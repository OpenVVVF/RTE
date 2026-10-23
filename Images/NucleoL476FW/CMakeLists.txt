cmake_minimum_required(VERSION 3.22)

#
# NucleoL476FW - minimal RTE base firmware for the ST Nucleo-L476RG.
#
# Mirrors the Images/Gen6FW CMake shape so RTEFirmwareBuilder can build it
# with --fw-src Images/NucleoL476FW (target name STM32CubeMX, .elf + .bin).
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name (kept as STM32CubeMX so RTEFirmwareBuilder's
# --target STM32CubeMX and ELF/BIN verification work unchanged)
set(CMAKE_PROJECT_NAME STM32CubeMX)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM, C and C++ languages
enable_language(C CXX ASM)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX-style generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Src/Inverter/InverterMain.cpp
    Src/Inverter/platform_api.cpp
    Src/Inverter/Drivers/PWM/pwm.cpp
)

# Add RTE-generated domain sources if they exist. These are produced by
# RTECodeEmitter and live in <firmware-root>/generated/.
file(GLOB GENERATED_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/*_generated.cpp
)
if(GENERATED_SOURCES)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${GENERATED_SOURCES})
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/generated
    )
endif()

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Inc
    Inc/Inverter
    Inc/Inverter/Drivers/PWM
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# Generate a raw binary after linking so the .bin is always in sync with the .elf.
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
    COMMENT "Generating ${CMAKE_PROJECT_NAME}.bin"
)
