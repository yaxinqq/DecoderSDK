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

# 查找Qt5
find_package(Qt5 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    OpenGL
    WebChannel
    WebSockets
)

# 全局包含目录设置
set(GLOBAL_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VideoPlayer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VideoPlayer/VideoRender
    ${FFMPEG_INCLUDE_DIR}
    ${SPDLOG_INCLUDE_DIR}
    ${FMT_INCLUDE_DIR}
    ${MAGIC_ENUM_INCLUDE_DIR}
    ${JSON_INCLUDE_DIR}
)

# 如果找到CUDA，添加CUDA包含目录
if(CUDA_FOUND)
    list(APPEND GLOBAL_INCLUDE_DIRS ${CUDA_INCLUDE_DIR})
endif()

# 全局链接库设置
set(GLOBAL_LINK_LIBRARIES
    DecoderSDK
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::OpenGL
    Qt5::WebChannel
    Qt5::WebSockets
)

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
    
    # Windows下的CUDA库
    if(CUDA_FOUND)
        list(APPEND GLOBAL_LINK_LIBRARIES
            cuda
            cudart
        )
        link_directories(${CUDA_LIB_DIR})
    endif()
elseif(UNIX AND NOT APPLE)
    # Linux特定库
    list(APPEND GLOBAL_LINK_LIBRARIES
        EGL
        X11
        pthread
    )
    
    # Linux下的CUDA库
    if(CUDA_FOUND)
        if(CUDAToolkit_FOUND)
            list(APPEND GLOBAL_LINK_LIBRARIES
                CUDA::cuda_driver
                CUDA::cudart
            )
        else()
            list(APPEND GLOBAL_LINK_LIBRARIES
                ${CUDA_LIBRARIES}
            )
        endif()
    endif()
elseif(APPLE)
    # macOS特定库
    find_library(OPENGL_LIBRARY OpenGL)
    find_library(COCOA_LIBRARY Cocoa)
    list(APPEND GLOBAL_LINK_LIBRARIES
        ${OPENGL_LIBRARY}
        ${COCOA_LIBRARY}
    )
    
    # macOS下的CUDA库
    if(CUDA_FOUND)
        if(CUDAToolkit_FOUND)
            list(APPEND GLOBAL_LINK_LIBRARIES
                CUDA::cuda_driver
                CUDA::cudart
            )
        else()
            list(APPEND GLOBAL_LINK_LIBRARIES
                ${CUDA_LIBRARIES}
            )
        endif()
    endif()
endif()

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/examples/qt_gui/Debug)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/examples/qt_gui/Release)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/examples/qt_gui/RelWithDebInfo)

# 添加主工程
add_subdirectory(src)