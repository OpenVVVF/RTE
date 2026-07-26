option(NODEGUI_ENABLE_FPS_OVERLAY "Show the FPS/frametime overlay in NodeGUI" ON)

add_executable(NodeGUI
    src/Main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/GraphScene.cpp
    src/GraphScene.h
    src/NodeDataModel.cpp
    src/NodeDataModel.h
    src/NodeGraphModel.cpp
    src/NodeGraphModel.h
    src/BridgeConnectionPainter.cpp
    src/BridgeConnectionPainter.h
    src/TypedNodePainter.cpp
    src/TypedNodePainter.h
    src/ParameterBlock.cpp
    src/ParameterBlock.h
    src/ParameterNodeGeometry.cpp
    src/ParameterNodeGeometry.h
    src/PortStyle.cpp
    src/PortStyle.h
    src/FrameRateMonitor.cpp
    src/FrameRateMonitor.h
)

set_target_properties(NodeGUI PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    AUTOUIC ON
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/QtNodes)

target_compile_definitions(NodeGUI
    PRIVATE
        RTE_NODE_TEMPLATES_DIR="${CMAKE_SOURCE_DIR}/Assets/NodeTemplates"
)

if(NODEGUI_ENABLE_FPS_OVERLAY)
    target_compile_definitions(NodeGUI PRIVATE NODEGUI_ENABLE_FPS_OVERLAY=1)
endif()


target_include_directories(NodeGUI
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/QtNodes/include/QtNodes/internal
)

target_link_libraries(NodeGUI
    PRIVATE
        NodeAPI
        QtNodes::QtNodes
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        project_warnings
)
