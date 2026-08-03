add_executable(RTEServer
    src/Main.cpp
    src/SerialBridge.cpp
    src/SerialBridge.h
)

set_target_properties(RTEServer PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(RTEServer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(RTEServer
    PRIVATE
        RTEShared
        InverterProtocol
        project_warnings
)
