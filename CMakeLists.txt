cmake_minimum_required(VERSION 3.14)

# 检测是否作为子项目被包含
if(NOT DEFINED PROJECT_NAME)
    set(DECODER_SDK_IS_MAIN_PROJECT TRUE)
else()
    set(DECODER_SDK_IS_MAIN_PROJECT FALSE)
endif()

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
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
else()
    set(GIT_HASH "unknown")
endif()

# 配置版本头文件
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/base/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
    @ONLY
)

# 添加选项控制是否编译examples（只在主项目时默认开启）
option(BUILD_EXAMPLES "Build example programs" ${DECODER_SDK_IS_MAIN_PROJECT})
option(BUILD_DECODERSDK_SHARED_LIBS "Build shared libraries" ON)

# 添加选项控制具体编译哪些例子
option(BUILD_CONSOLE_EXAMPLE "Build console example" OFF)
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
# set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")

# 设置安装路径（只在主项目时设置）
if(DECODER_SDK_IS_MAIN_PROJECT AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/install" CACHE PATH "Install path" FORCE)
endif()

# 设置输出目录（只在主项目时设置）
if(DECODER_SDK_IS_MAIN_PROJECT)
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
endif()

# 获取系统信息
message(STATUS "CMake system name: ${CMAKE_SYSTEM_NAME}")
message(STATUS "CMake system processor: ${CMAKE_SYSTEM_PROCESSOR}")

# 根据系统和架构设置包含目录和库文件路径
#eventpp
set(EVENTPP_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/eventpp/include)
#nlohmann
set(JSON_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann/include)
# magic_enum
set(MAGIC_ENUM_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/magic_enum/include)
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # Windows-x86_64
    # FFmpeg
    set(FFMPEG_INCLUDE_DIR ${FFMPEG_ROOT_DIR}/include)
    set(FFMPEG_LIB_DIR ${FFMPEG_ROOT_DIR}/lib)
    set(FFMPEG_BIN_DIR ${FFMPEG_ROOT_DIR}/bin)
    # spdlog
    set(SPDLOG_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/spdlog/windows-x86_64/include)
    set(SPDLOG_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/spdlog/windows-x86_64/lib)

    # fmt
    set(FMT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/fmt/windows-x86_64/include)
    set(FMT_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/fmt/windows-x86_64/lib)

elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(FFMPEG_INCLUDE_DIR )
    set(FFMPEG_LIB_DIR  )
    set(FFMPEG_BIN_DIR )

    # FFmpeg配置 - 优先使用FFMPEG_ROOT_DIR，否则使用pkg-config收集各模块路径
    if(FFMPEG_ROOT_DIR AND EXISTS "${FFMPEG_ROOT_DIR}")
        # 方案1: 使用用户指定的FFMPEG_ROOT_DIR
        message(STATUS "Using user-specified FFMPEG_ROOT_DIR: ${FFMPEG_ROOT_DIR}")
        
        # 设置统一的路径
        set(FFMPEG_INCLUDE_DIR ${FFMPEG_ROOT_DIR}/include)
        set(FFMPEG_LIB_DIR ${FFMPEG_ROOT_DIR}/lib)
        set(FFMPEG_BIN_DIR ${FFMPEG_LIB_DIR})
        
        message(STATUS "FFmpeg include directories: ${FFMPEG_INCLUDE_DIR}")
        message(STATUS "FFmpeg lib directories: ${FFMPEG_LIB_DIR}")
        message(STATUS "FFmpeg bin directory: ${FFMPEG_BIN_DIR}")
        
    else()
        # 方案2: 使用pkg-config收集各个模块的路径
        message(STATUS "FFMPEG_ROOT_DIR not set, using pkg-config to collect FFmpeg module paths")
        
        find_package(PkgConfig REQUIRED)
        
        # 定义FFmpeg模块列表
        set(FFMPEG_MODULES 
            "libavcodec"
            "libavdevice" 
            "libavfilter"
            "libavformat"
            "libavutil"
            "libswresample"
            "libswscale"
        )
        
        # 收集各个模块的头文件和库文件路径
        foreach(module ${FFMPEG_MODULES})
            # 检查模块是否存在
            pkg_check_modules(${module}_PKG REQUIRED ${module})
            
            # 收集头文件路径
            if(${module}_PKG_INCLUDE_DIRS)
                foreach(include_dir ${${module}_PKG_INCLUDE_DIRS})
                    list(APPEND FFMPEG_INCLUDE_DIR ${include_dir})
                endforeach()
            endif()
            
            # 收集库文件路径
            if(${module}_PKG_LIBRARY_DIRS)
                foreach(lib_dir ${${module}_PKG_LIBRARY_DIRS})
                    list(APPEND FFMPEG_LIB_DIR ${lib_dir})
                endforeach()
            endif()
            
            # 通过pkg-config获取具体的库目录
            execute_process(
                COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=libdir ${module}
                OUTPUT_VARIABLE ${module}_LIBDIR
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            
            if(${module}_LIBDIR)
                list(APPEND FFMPEG_LIB_DIR ${${module}_LIBDIR})
                message(STATUS "Found ${module} lib directory: ${${module}_LIBDIR}")
            endif()
        endforeach()
        
        # 去重路径数组
        if(FFMPEG_INCLUDE_DIR)
            list(REMOVE_DUPLICATES FFMPEG_INCLUDE_DIR)
        endif()
        if(FFMPEG_LIB_DIR)
            list(REMOVE_DUPLICATES FFMPEG_LIB_DIR)
        endif()
        # linux下，lib和bin的目录一致
        set(FFMPEG_BIN_DIR ${FFMPEG_LIB_DIR})
    endif()

    # 输出最终的路径信息
    message(STATUS "=== FFmpeg Configuration Summary ===")
    message(STATUS "  Include Dirs: ${FFMPEG_INCLUDE_DIR}")
    message(STATUS "  Library Dirs: ${FFMPEG_LIB_DIR}")
    message(STATUS "  Binary Dir:   ${FFMPEG_BIN_DIR}")
    message(STATUS "=====================================")

    # spdlog
    set(SPDLOG_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/spdlog/linux-${CMAKE_SYSTEM_PROCESSOR}/include)
    set(SPDLOG_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/spdlog/linux-${CMAKE_SYSTEM_PROCESSOR}/lib)

    # fmt
    set(FMT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/fmt/linux-${CMAKE_SYSTEM_PROCESSOR}/include)
    set(FMT_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/fmt/linux-${CMAKE_SYSTEM_PROCESSOR}/lib)


else ()
    # 不支持的系统或处理器架构
    message(FATAL_ERROR "Unsupported system ${CMAKE_SYSTEM_NAME} or unsupported processor ${CMAKE_SYSTEM_PROCESSOR}")
endif ()

# 如果不是作为主项目，则设置依赖库的路径
if(NOT DECODER_SDK_IS_MAIN_PROJECT)
    set(DECODERSDK_DEPENDENCY_DIR ${FFMPEG_BIN_DIR} PARENT_SCOPE)
endif()

# 查找和配置各种硬件解码支持
set(CUDA_FOUND FALSE)
set(D3D11VA_FOUND FALSE)
set(DXVA2_FOUND FALSE)
set(VAAPI_FOUND FALSE)
set(VDPAU_FOUND FALSE)
set(OPENMAX_FOUND FALSE)
set(VIDEOTOOLBOX_FOUND FALSE)
set(QSV_FOUND FALSE)
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

# 创建别名目标，方便作为子项目使用
add_library(DecoderSDK::DecoderSDK ALIAS DecoderSDK)

# examples (可选)
if(BUILD_EXAMPLES)
    message(STATUS "Building examples")
    add_subdirectory(examples)
else()
    message(STATUS "Skipping examples")
endif()

# 只在主项目时进行安装配置
if(DECODER_SDK_IS_MAIN_PROJECT)
    # 创建配置文件
    include(CMakePackageConfigHelpers)
    
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DecoderSDKConfig.cmake.in"
        "${CMAKE_BINARY_DIR}/DecoderSDKConfig.cmake"
        INSTALL_DESTINATION lib/cmake/DecoderSDK
        PATH_VARS CMAKE_INSTALL_PREFIX
    )

    # 安装配置文件
    install(FILES
        "${CMAKE_BINARY_DIR}/DecoderSDKConfig.cmake"
        DESTINATION lib/cmake/DecoderSDK
    )

    # 安装配置文件
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/decoderSDK.json"
        DESTINATION etc
    )

    # 安装文档
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        DESTINATION .
    )
endif()