# src/CMakeLists.txt

# 定义工程名称变量
set(PROJECT_NAME DecoderSDK)

# 模块目录配置
set(MODULE_DIRS
    api
    base
    decoder
    demuxer
    event_system
    include
    logger
    recorder
    stream_sync
    utils
)

# 源文件收集
foreach(MODULE ${MODULE_DIRS})
    string(TOUPPER ${MODULE} MODULE_UPPER)
    file(GLOB ${MODULE_UPPER}_SRC
        "${MODULE}/*.h"
        "${MODULE}/*.cpp"
        "${MODULE}/*.hpp"
        "${MODULE}/*.cc"
    )
    source_group(${MODULE} FILES ${${MODULE_UPPER}_SRC})
    list(APPEND ALL_MODULE_SOURCES ${${MODULE_UPPER}_SRC})
endforeach()

# 根目录源文件收集
file(GLOB ROOT_SRC
    "*.h"
    "*.cpp"
    "*.hpp"
    "*.cc"
)

# 定义库类型
if(BUILD_SHARED_LIBS)
    set(LIB_TYPE SHARED)
else()
    set(LIB_TYPE STATIC)
endif()

# 定义库
add_library(${PROJECT_NAME} ${LIB_TYPE}
    ${ROOT_SRC}
    ${ALL_MODULE_SOURCES}
)

# 设置Debug模式下的库文件名后缀
set_target_properties(${PROJECT_NAME} PROPERTIES
    DEBUG_POSTFIX "d"
    VERSION "1.0.0"
    SOVERSION "1"
)

# 设置导出符号（Windows DLL）
if(WIN32 AND BUILD_SHARED_LIBS)
    target_compile_definitions(${PROJECT_NAME}
        PRIVATE DECODER_SDK_LIB
        INTERFACE DECODER_SDK_DLL
    )
endif()

# 包含目录
target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/external
        ${FFMPEG_INCLUDE_DIR}
        ${SPDLOG_INCLUDE_DIR}
        ${JSON_INCLUDE_DIR}
        ${FMT_INCLUDE_DIR}
        ${EVENTPP_INCLUDE_DIR}
        ${MAGIC_ENUM_INCLUDE_DIR}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/include>
        $<INSTALL_INTERFACE:include>
)

# 添加库文件路径
target_link_directories(${PROJECT_NAME}
    PRIVATE
        ${FFMPEG_LIB_DIR}
        ${SPDLOG_LIB_DIR}
        ${FMT_LIB_DIR}
)

# ffmpeg不区分debug和release
set(FFMPEG_LIB
    avcodec
    avdevice
    avfilter
    avformat
    avutil
    swresample
    swscale
)
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND FFMPEG_LIB
        postproc
    )
endif ()

set(SPDLOG_LIB spdlog)
set(FMT_LIB spdlog)

# 链接库
target_link_libraries(${PROJECT_NAME}
    PRIVATE
        ${FFMPEG_LIB}
        ${SPDLOG_LIB}
        ${FMT_LIB}
)

# 编译定义
target_compile_definitions(${PROJECT_NAME}
    PRIVATE
        DECODER_SDK_LIB
)

# 安装目标
install(TARGETS ${PROJECT_NAME}
    EXPORT DecoderSDKTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    INCLUDES DESTINATION include
)

# 安装头文件
install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

# 安装 PDB 文件（Windows Debug信息）
if (MSVC AND BUILD_SHARED_LIBS)
    install(FILES "$<TARGET_PDB_FILE:${PROJECT_NAME}>"
        DESTINATION bin
        CONFIGURATIONS Debug RelWithDebInfo Release
        OPTIONAL
    )
endif ()

# 导出目标
install(EXPORT DecoderSDKTargets
    FILE DecoderSDKTargets.cmake
    NAMESPACE DecoderSDK::
    DESTINATION lib/cmake/DecoderSDK
)

# 生成版本文件
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_BINARY_DIR}/DecoderSDKConfigVersion.cmake"
    VERSION "1.0.0"
    COMPATIBILITY SameMajorVersion
)

# 安装版本文件
install(FILES
    "${CMAKE_BINARY_DIR}/DecoderSDKConfigVersion.cmake"
    DESTINATION lib/cmake/DecoderSDK
)