# src/CMakeLists.txt

# 定义工程名称变量
set(PROJECT_NAME DecoderSDK)

# Windows特定配置
if(WIN32)
    # 启用资源编译器
    enable_language(RC)
endif()

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

# 硬件加速文件收集
set(ALL_HWACCEL_SRC )
set(HWACCEL_DIRS )
if (VAAPI_FOUND)
    # 模块目录配置
    list(APPEND HWACCEL_DIRS
        vaapi
    )
endif()

# 硬件相关源文件收集
foreach(HWACCEL ${HWACCEL_DIRS})
    string(TOUPPER ${HWACCEL} HWACCEL_UPPER)
    file(GLOB ${HWACCEL_UPPER}_SRC
        "${HWACCEL}/*.h"
        "${HWACCEL}/*.cpp"
        "${HWACCEL}/*.hpp"
        "${HWACCEL}/*.cc"
    )
    source_group(${HWACCEL} FILES ${${HWACCEL_UPPER}_SRC})
    list(APPEND ALL_HWACCEL_SRC ${${HWACCEL_UPPER}_SRC})
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
    ${ALL_HWACCEL_SRC}
    # 添加Windows资源文件
    $<$<PLATFORM_ID:Windows>:DecoderSDK.rc>
)

# Windows特定配置
if(WIN32)
    # 启用资源编译器
    enable_language(RC)
    
    # 设置资源文件的包含目录
    set_property(SOURCE DecoderSDK.rc PROPERTY
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()

# 设置库版本属性
set_target_properties(${PROJECT_NAME} PROPERTIES
    VERSION "${DECODER_SDK_VERSION_MAJOR}.${DECODER_SDK_VERSION_MINOR}.${DECODER_SDK_VERSION_PATCH}"
    SOVERSION "${DECODER_SDK_VERSION_MAJOR}"
    DEBUG_POSTFIX "d"
)

# Linux特定的版本信息
if(UNIX AND NOT APPLE)
    # 设置RPATH
    set_target_properties(${PROJECT_NAME} PROPERTIES
        INSTALL_RPATH_USE_LINK_PATH TRUE
    )
endif()

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
        ${CMAKE_BINARY_DIR}/generated
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

# 如果CUDA可用，添加CUDA包含目录
if(CUDA_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${CUDA_INCLUDE_DIR})
endif()

# 如果QSV可用，添加QSV包含目录
if (QSV_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${QSV_INCLUDE_DIR})
endif()

# 如果VAAPI可用，添加VAAPI包含目录
if(VAAPI_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${LIBVA_INCLUDE_DIR})
endif()

# 如果VDPAU可用，添加VDPAU包含目录
if(VDPAU_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${VDPAU_INCLUDE_DIR})
endif()


# 添加库文件路径
target_link_directories(${PROJECT_NAME}
    PRIVATE
        ${FFMPEG_LIB_DIR}
        ${SPDLOG_LIB_DIR}
        ${FMT_LIB_DIR}
)

# 如果CUDA可用，添加CUDA库目录
if(CUDA_FOUND)
    target_link_directories(${PROJECT_NAME} PRIVATE ${CUDA_LIB_DIR})
endif()

# 如果QSV可用，添加QSV库目录
if (QSV_FOUND)
    target_link_directories(${PROJECT_NAME} PRIVATE ${QSV_LIB_DIR})
endif()

# 如果VAAPI可用，添加VAAPI库目录
if(VAAPI_FOUND)
    target_link_directories(${PROJECT_NAME} PRIVATE ${LIBVA_LIB_DIR})
endif()

# 如果VDPAU可用，添加VDPAU库目录
if(VDPAU_FOUND)
    target_link_directories(${PROJECT_NAME} PRIVATE ${VDPAU_LIB_DIR})
endif()


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
set(FMT_LIB fmt)

# 硬件加速相关库
set(HARDWARE_ACCEL_LIBS)

# CUDA库
if(CUDA_FOUND)
    list(APPEND HARDWARE_ACCEL_LIBS
        cuda
    )
endif()

# Windows平台硬件加速库
if(WIN32)
    list(APPEND HARDWARE_ACCEL_LIBS
        # D3D11相关
        d3d11
        dxgi
        # DXVA2相关
        dxva2
    )
    
    # Intel Media SDK库（QSV）
    if(QSV_FOUND)
        list(APPEND HARDWARE_ACCEL_LIBS
            libmfx
        )
    endif()
    
elseif(UNIX AND NOT APPLE)
    # Linux平台硬件加速库
    
    # VAAPI库
    if(VAAPI_FOUND)
        list(APPEND HARDWARE_ACCEL_LIBS
            ${LIBVA_LIBRARIES}
        )
        if(LIBVA_DRM_FOUND)
            list(APPEND HARDWARE_ACCEL_LIBS ${LIBVA_DRM_LIBRARIES})
        endif()
        if(LIBVA_X11_FOUND)
            list(APPEND HARDWARE_ACCEL_LIBS ${LIBVA_X11_LIBRARIES})
        endif()
    endif()
    
    # VDPAU库
    if(VDPAU_FOUND)
        list(APPEND HARDWARE_ACCEL_LIBS ${VDPAU_LIBRARIES})
    endif()
    
    # Intel Media SDK库（QSV）
    if(QSV_FOUND)
        list(APPEND HARDWARE_ACCEL_LIBS mfx)
    endif()
    
elseif(APPLE)
    # macOS平台硬件加速库
    list(APPEND HARDWARE_ACCEL_LIBS
        # VideoToolbox框架
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreFoundation"
    )
endif()

# 链接库
target_link_libraries(${PROJECT_NAME}
    PRIVATE
        ${FFMPEG_LIB}
        ${SPDLOG_LIB}
        ${FMT_LIB}
        ${HARDWARE_ACCEL_LIBS}
)

if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    target_link_libraries(${PROJECT_NAME} PRIVATE stdc++fs)
endif()

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