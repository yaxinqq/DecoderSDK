cmake_minimum_required(VERSION 3.14)
project(DecoderSDK)

# Only generate Debug and Release configuration types.
set(CMAKE_CONFIGURATION_TYPES Debug Release RelWithDebInfo)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release")
endif ()

# 在项目开始处定义版本信息
set(DECODER_SDK_VERSION_MAJOR 1)
set(DECODER_SDK_VERSION_MINOR 0)
set(DECODER_SDK_VERSION_PATCH 0)
set(DECODER_SDK_VERSION_BUILD 0)

# 获取构建时间和Git信息
string(TIMESTAMP BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP BUILD_TIME "%H:%M:%S")

# 获取Git哈希值（可选）
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
else()
    set(GIT_HASH "unknown")
endif()

# 配置版本头文件
configure_file(
    "${CMAKE_SOURCE_DIR}/src/base/version.h.in"
    "${CMAKE_BINARY_DIR}/generated/version.h"
    @ONLY
)

# 添加选项控制是否编译examples
option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

# 添加选项控制具体编译哪些例子
option(BUILD_CONSOLE_EXAMPLE "Build console example" ON)
option(BUILD_QT_GUI_EXAMPLE "Build Qt GUI example" ON)
option(BUILD_D3D_RENDER_EXAMPLE "Build D3D render example" OFF)

# 设置FFMpeg的根路径
set(FFMPEG_ROOT_DIR "" CACHE PATH "Path to FFmpeg installation directory")

# 设置编译选项
if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/Zc:__cplusplus)
    add_compile_options(/MP)
    add_compile_options(/utf-8)
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    add_definitions(-DUNICODE -D_UNICODE)

    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")                         # 为Release模式添加调试信息
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG")  # 链接器生成dll的PDB
endif ()

# 添加自定义的CMake模块路径
# set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")

# 设置安装路径
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install path" FORCE)
endif()

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
# set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)

# 针对不同的构建类型设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/output/Debug)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/output/Release)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/output/RelWithDebInfo)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/output/Debug)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/output/Release)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/output/RelWithDebInfo)

# 获取系统信息
message(STATUS "CMake system name: ${CMAKE_SYSTEM_NAME}")
message(STATUS "CMake system processor: ${CMAKE_SYSTEM_PROCESSOR}")

# 根据系统和架构设置包含目录和库文件路径
#eventpp
set(EVENTPP_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/external/eventpp/include)
#nlohmann
set(JSON_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/external/nlohmann/include)
# magic_enum
set(MAGIC_ENUM_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/external/magic_enum/include)
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # Windows-x86_64
    # FFmpeg
    set(FFMPEG_INCLUDE_DIR ${FFMPEG_ROOT_DIR}/include)
    set(FFMPEG_LIB_DIR ${FFMPEG_ROOT_DIR}/lib)
    set(FFMPEG_BIN_DIR ${FFMPEG_ROOT_DIR}/bin)
    # spdlog
    set(SPDLOG_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/external/spdlog/windows-x86_64/include)
    set(SPDLOG_LIB_DIR ${CMAKE_SOURCE_DIR}/external/spdlog/windows-x86_64/lib)
    set(SPDLOG_BIN_DIR ${CMAKE_SOURCE_DIR}/external/spdlog/windows-x86_64/bin)

    # fmt
    set(FMT_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/external/fmt/windows-x86_64/include)
    set(FMT_LIB_DIR ${CMAKE_SOURCE_DIR}/external/fmt/windows-x86_64/lib)

elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    # like Kylin-aarch64
    

elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    # like UOS Desktop 20
    # FFmpeg
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(AVCODEC REQUIRED libavcodec)
    pkg_check_modules(AVDEVICE REQUIRED libavdevice)
    pkg_check_modules(AVFILTER REQUIRED libavfilter)
    pkg_check_modules(AVFORMAT REQUIRED libavformat)
    pkg_check_modules(AVUTIL REQUIRED libavutil)
    pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
    pkg_check_modules(SWSCALE REQUIRED libswscale)
    # spdlog
    set(SPDLOG_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/external/spdlog/linux-x86_64/include)
    set(SPDLOG_LIB_DIR ${CMAKE_SOURCE_DIR}/external/spdlog/linux-x86_64/lib)

    # fmt
    set(FMT_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/external/fmt/linux-x86_64/include)
    set(FMT_LIB_DIR ${CMAKE_SOURCE_DIR}/external/fmt/linux-x86_64/lib)

else ()
    # 不支持的系统或处理器架构
    message(FATAL_ERROR "Unsupported system ${CMAKE_SYSTEM_NAME} or unsupported processor ${CMAKE_SYSTEM_PROCESSOR}")
endif ()

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
    set(D3D11VA_FOUND TRUE)
    add_definitions(-DD3D11VA_AVAILABLE)
    message(STATUS "D3D11VA hardware acceleration enabled")
    
    # DXVA2支持检测（Windows默认支持）
    set(DXVA2_FOUND TRUE)
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
        # 用老版工具再检测一次
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
    set(VIDEOTOOLBOX_FOUND TRUE)
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

# 添加子项目
# src
add_subdirectory(src)
# examples (可选)
if(BUILD_EXAMPLES)
    message(STATUS "Building examples")
    add_subdirectory(examples)
else()
    message(STATUS "Skipping examples")
endif()

# 创建配置文件
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/DecoderSDKConfig.cmake.in"
    "${CMAKE_BINARY_DIR}/DecoderSDKConfig.cmake"
    @ONLY
)

# 安装配置文件
install(FILES
    "${CMAKE_BINARY_DIR}/DecoderSDKConfig.cmake"
    DESTINATION lib/cmake/DecoderSDK
)

# 安装依赖的DLL文件（Windows）
if(WIN32)
    install(DIRECTORY ${FFMPEG_BIN_DIR}/
        DESTINATION bin
        FILES_MATCHING PATTERN "*.dll"
    )
    install(DIRECTORY ${SPDLOG_BIN_DIR}/
        DESTINATION bin
        FILES_MATCHING PATTERN "*.dll"
    )
endif()

# 安装配置文件
install(FILES
    "${CMAKE_SOURCE_DIR}/resources/decoderSDK.json"
    DESTINATION etc
)

# 安装文档
install(FILES
    "${CMAKE_SOURCE_DIR}/README.md"
    DESTINATION .
)