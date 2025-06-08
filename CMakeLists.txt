cmake_minimum_required(VERSION 3.15)
project(DecoderSDK)

# Only generate Debug and Release configuration types.
set(CMAKE_CONFIGURATION_TYPES Debug Release RelWithDebInfo)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release")
endif ()

# 全局设置
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install")

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
# magic_enum
set(MAGIC_ENUM_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/external/magic_enum/include)
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # Windows-x86_64
    # FFmpeg
    set(FFMPEG_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/external/ffmpeg/windows-x86_64/include)
    set(FFMPEG_LIB_DIR ${CMAKE_SOURCE_DIR}/external/ffmpeg/windows-x86_64/lib)
    set(FFMPEG_BIN_DIR ${CMAKE_SOURCE_DIR}/external/ffmpeg/windows-x86_64/bin)
    # spdlog
    set(SPDLOG_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/external/spdlog/windows-x86_64/include)
    set(SPDLOG_LIB_DIR ${CMAKE_SOURCE_DIR}/external/spdlog/windows-x86_64/lib)
    set(SPDLOG_BIN_DIR ${CMAKE_SOURCE_DIR}/external/spdlog/windows-x86_64/bin)

    # fmt
    set(FMT_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/external/fmt/windows-x86_64/include)
    set(FMT_LIB_DIR ${CMAKE_SOURCE_DIR}/external/fmt/windows-x86_64/lib)

elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    # Kylin-aarch64
    

elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    # UOS Desktop 20


else ()
    # 不支持的系统或处理器架构
    message(FATAL_ERROR "Unsupported system ${CMAKE_SYSTEM_NAME} or unsupported processor ${CMAKE_SYSTEM_PROCESSOR}")
endif ()

set(JSON_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/external/nlohmann/include)

# Linux 下检测操作系统的发行信息
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # 查询操作系统的发行信息
    cmake_host_system_information(RESULT DISTRO QUERY DISTRIB_ID DISTRIB_DESCRIPTION)
    # 输出获取的操作系统信息
    message(STATUS "Operating System Information: ${DISTRO}")
    # 检测统信 UOS
    if (DISTRO MATCHES "uos")
        set(OS_VENDOR "UOS")
        message(STATUS "Detected OS: 统信UOS")
        add_definitions(-DOS_UOS)  # 添加预处理宏
    # 检测银河麒麟
    elseif (DISTRO MATCHES "kylin")
        set(OS_VENDOR "Kylin")
        message(STATUS "Detected OS: 银河麒麟")
        add_definitions(-DOS_KYLIN)  # 添加预处理宏
    else()
        set(OS_VENDOR "Unknown")
        message(WARNING "Unknown OS vendor detected: ${DISTRO}")
    endif()
endif()

# 添加子项目
# src
add_subdirectory(src)
# external
add_subdirectory(external)
# resources
add_subdirectory(resources)