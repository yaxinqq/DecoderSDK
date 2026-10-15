cmake_minimum_required(VERSION 3.16)

project(qt_gui_example)

# 设置Qt相关选项
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置构建类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

# 设置编译选项
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/Zc:__cplusplus)
    add_compile_options(/MP)
    add_compile_options(/utf-8)
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    add_definitions(-DUNICODE -D_UNICODE)
    
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG")
endif()

# 优先查找 Qt6，找不到时回退到 Qt5
set(QT_GUI_QT_COMPONENTS
    Core
    Gui
    Widgets
    Multimedia
    OpenGL
)
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS ${QT_GUI_QT_COMPONENTS})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS ${QT_GUI_QT_COMPONENTS})

# Qt6 的 QOpenGLWidget 位于独立的 OpenGLWidgets 模块
if(QT_VERSION_MAJOR EQUAL 6)
    find_package(Qt6 REQUIRED COMPONENTS OpenGLWidgets)
endif()

# QRhiWidget 从 Qt 6.7 开始提供，Qt5 和更早的 Qt6 不编译相关播放器
set(QT_GUI_ENABLE_QRHI_PLAYER FALSE)
if(QT_VERSION_MAJOR EQUAL 6 AND Qt6_VERSION VERSION_GREATER_EQUAL 6.7)
    set(QT_NO_PRIVATE_MODULE_WARNING ON)
    find_package(Qt6 QUIET COMPONENTS GuiPrivate)
    if(TARGET Qt6::GuiPrivate)
        set(QT_GUI_ENABLE_QRHI_PLAYER TRUE)
    endif()
endif()

message(STATUS "Qt GUI QRhi player: ${QT_GUI_ENABLE_QRHI_PLAYER}")

# 全局包含目录设置
set(GLOBAL_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VideoPlayer/Base
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VideoPlayer/Decoder
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VideoPlayer/Player
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VideoPlayer/Renderer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VideoPlayer/Stream
    ${CMAKE_CURRENT_SOURCE_DIR}/external/vk_bootstrap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/vma/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/vulkan/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/vpl/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/amf/include
)

# 如果找到CUDA，添加CUDA包含目录
if(CUDA_FOUND)
    list(APPEND GLOBAL_INCLUDE_DIRS ${CUDA_INCLUDE_DIR})
endif()

# 全局链接库设置
set(GLOBAL_LINK_LIBRARIES
    DecoderSDK
)

foreach(QT_COMPONENT IN LISTS QT_GUI_QT_COMPONENTS)
    list(APPEND GLOBAL_LINK_LIBRARIES Qt${QT_VERSION_MAJOR}::${QT_COMPONENT})
endforeach()

if(QT_VERSION_MAJOR EQUAL 6)
    list(APPEND GLOBAL_LINK_LIBRARIES Qt6::OpenGLWidgets)

    if(QT_GUI_ENABLE_QRHI_PLAYER)
        list(APPEND GLOBAL_LINK_LIBRARIES Qt6::GuiPrivate)
    endif()
endif()

# 平台特定的链接库设置
if(WIN32)
    # Windows特定库
    list(APPEND GLOBAL_LINK_LIBRARIES
        opengl32
        glu32
        d3d9
        d3d11
        dxgi
        d3dcompiler
        dxva2
    )
elseif(UNIX AND NOT APPLE)
    # Linux特定库
    list(APPEND GLOBAL_LINK_LIBRARIES
        EGL
        X11
        pthread
    )
elseif(APPLE)
    # macOS特定库
    find_library(OPENGL_LIBRARY OpenGL)
    find_library(COCOA_LIBRARY Cocoa)
    list(APPEND GLOBAL_LINK_LIBRARIES
        ${OPENGL_LIBRARY}
        ${COCOA_LIBRARY}
    )
endif()

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/examples/qt_gui/Debug)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/examples/qt_gui/Release)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/examples/qt_gui/RelWithDebInfo)

# 添加主工程
add_subdirectory(src)
