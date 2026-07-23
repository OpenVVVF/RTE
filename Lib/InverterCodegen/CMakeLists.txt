add_library(InverterCodegenLib STATIC
    src/CodeGenerator.cpp
)

target_include_directories(InverterCodegenLib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(InverterCodegenLib
    PUBLIC NodeAPI
    PRIVATE project_warnings
)

add_executable(InverterCodegen
    src/Main.cpp
)

target_link_libraries(InverterCodegen
    PRIVATE InverterCodegenLib project_warnings
)
