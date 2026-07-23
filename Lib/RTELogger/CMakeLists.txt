add_library(RTELogger STATIC
    src/Logger.cpp
)

target_include_directories(RTELogger
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(RTELogger
    PRIVATE project_warnings
)
