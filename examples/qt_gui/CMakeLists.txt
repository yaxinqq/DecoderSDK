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

# 查找和配置CUDA
# 查找和配置各种硬件解码支持
if(WIN32)
    # Windows平台硬件解码支持
    
    # CUDA支持检测
    if(DEFINED ENV{CUDA_PATH})
        set(CUDA_ROOT_DIR $ENV{CUDA_PATH})
        set(CUDA_INCLUDE_DIR "${CUDA_ROOT_DIR}/include")
        set(CUDA_LIB_DIR "${CUDA_ROOT_DIR}/lib/x64")
        
        if(EXISTS "${CUDA_INCLUDE_DIR}/cuda.h")
            message(STATUS "Found CUDA at: ${CUDA_ROOT_DIR}")
            set(CUDA_FOUND TRUE)
            add_definitions(-DCUDA_AVAILABLE)
        else()
            message(WARNING "CUDA headers not found at: ${CUDA_INCLUDE_DIR}")
            set(CUDA_FOUND FALSE)
        endif()
    else()
        message(WARNING "CUDA_PATH environment variable not set. CUDA features will be disabled.")
        set(CUDA_FOUND FALSE)
    endif()
    
    # D3D11VA支持检测（Windows默认支持）
    add_definitions(-DD3D11VA_AVAILABLE)
    message(STATUS "D3D11VA hardware acceleration enabled")
    
    # DXVA2支持检测（Windows默认支持）
    add_definitions(-DDXVA2_AVAILABLE)
    message(STATUS "DXVA2 hardware acceleration enabled")
    
    # DirectX Video Acceleration支持
    add_definitions(-DDIRECTX_VA_AVAILABLE)
    message(STATUS "DirectX Video Acceleration enabled")
    
elseif(UNIX AND NOT APPLE)
    # Linux平台硬件解码支持
    
    # CUDA支持检测
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        set(CUDA_FOUND TRUE)
        set(CUDA_INCLUDE_DIR ${CUDAToolkit_INCLUDE_DIRS})
        add_definitions(-DCUDA_AVAILABLE)
        message(STATUS "Found CUDA Toolkit: ${CUDAToolkit_VERSION}")
    else()
        find_package(CUDA QUIET)
        if(CUDA_FOUND)
            set(CUDA_INCLUDE_DIR ${CUDA_INCLUDE_DIRS})
            add_definitions(-DCUDA_AVAILABLE)
            message(STATUS "Found CUDA: ${CUDA_VERSION}")
        else()
            message(WARNING "CUDA not found. CUDA features will be disabled.")
            set(CUDA_FOUND FALSE)
        endif()
    endif()
    
    # VAAPI支持检测（Video Acceleration API for Linux）
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBVA QUIET libva)
        pkg_check_modules(LIBVA_DRM QUIET libva-drm)
        pkg_check_modules(LIBVA_X11 QUIET libva-x11)
        
        if(LIBVA_FOUND)
            add_definitions(-DVAAPI_AVAILABLE)
            message(STATUS "Found VAAPI: ${LIBVA_VERSION}")
            set(VAAPI_FOUND TRUE)
        else()
            message(WARNING "VAAPI not found. VAAPI features will be disabled.")
            set(VAAPI_FOUND FALSE)
        endif()
    else()
        message(WARNING "PkgConfig not found. Cannot detect VAAPI.")
        set(VAAPI_FOUND FALSE)
    endif()
    
    # VDPAU支持检测（Video Decode and Presentation API for Unix）
    if(PkgConfig_FOUND)
        pkg_check_modules(VDPAU QUIET vdpau)
        if(VDPAU_FOUND)
            add_definitions(-DVDPAU_AVAILABLE)
            message(STATUS "Found VDPAU: ${VDPAU_VERSION}")
            set(VDPAU_FOUND TRUE)
        else()
            message(WARNING "VDPAU not found. VDPAU features will be disabled.")
            set(VDPAU_FOUND FALSE)
        endif()
    else()
        set(VDPAU_FOUND FALSE)
    endif()
    
    # OpenMAX支持检测（主要用于嵌入式设备）
    find_path(OPENMAX_INCLUDE_DIR
        NAMES IL/OMX_Core.h
        PATHS /usr/include /usr/local/include
    )
    if(OPENMAX_INCLUDE_DIR)
        add_definitions(-DOPENMAX_AVAILABLE)
        message(STATUS "Found OpenMAX IL headers")
        set(OPENMAX_FOUND TRUE)
    else()
        message(STATUS "OpenMAX IL not found")
        set(OPENMAX_FOUND FALSE)
    endif()
    
elseif(APPLE)
    # macOS平台硬件解码支持
    
    # CUDA支持检测
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        set(CUDA_FOUND TRUE)
        set(CUDA_INCLUDE_DIR ${CUDAToolkit_INCLUDE_DIRS})
        add_definitions(-DCUDA_AVAILABLE)
        message(STATUS "Found CUDA Toolkit: ${CUDAToolkit_VERSION}")
    else()
        find_package(CUDA QUIET)
        if(CUDA_FOUND)
            set(CUDA_INCLUDE_DIR ${CUDA_INCLUDE_DIRS})
            add_definitions(-DCUDA_AVAILABLE)
            message(STATUS "Found CUDA: ${CUDA_VERSION}")
        else()
            message(WARNING "CUDA not found. CUDA features will be disabled.")
            set(CUDA_FOUND FALSE)
        endif()
    endif()
    
    # VideoToolbox支持检测（macOS默认支持）
    add_definitions(-DVIDEOTOOLBOX_AVAILABLE)
    message(STATUS "VideoToolbox hardware acceleration enabled")
    
    # Metal Performance Shaders支持检测
    find_library(METAL_LIBRARY Metal)
    if(METAL_LIBRARY)
        add_definitions(-DMETAL_AVAILABLE)
        message(STATUS "Metal Performance Shaders enabled")
        set(METAL_FOUND TRUE)
    else()
        message(WARNING "Metal framework not found")
        set(METAL_FOUND FALSE)
    endif()
    
endif()

# 输出硬件解码支持摘要
message(STATUS "Hardware Acceleration Support Summary:")
if(CUDA_FOUND)
    message(STATUS "  - CUDA: Enabled")
else()
    message(STATUS "  - CUDA: Disabled")
endif()

if(WIN32)
    message(STATUS "  - D3D11VA: Enabled")
    message(STATUS "  - DXVA2: Enabled")
    message(STATUS "  - DirectX VA: Enabled")
elseif(UNIX AND NOT APPLE)
    if(VAAPI_FOUND)
        message(STATUS "  - VAAPI: Enabled")
    else()
        message(STATUS "  - VAAPI: Disabled")
    endif()
    if(VDPAU_FOUND)
        message(STATUS "  - VDPAU: Enabled")
    else()
        message(STATUS "  - VDPAU: Disabled")
    endif()
    if(OPENMAX_FOUND)
        message(STATUS "  - OpenMAX: Enabled")
    else()
        message(STATUS "  - OpenMAX: Disabled")
    endif()
elseif(APPLE)
    message(STATUS "  - VideoToolbox: Enabled")
    if(METAL_FOUND)
        message(STATUS "  - Metal: Enabled")
    else()
        message(STATUS "  - Metal: Disabled")
    endif()
endif()

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
        GL
        GLU
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