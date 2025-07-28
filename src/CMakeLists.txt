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
if(BUILD_DECODERSDK_SHARED_LIBS)
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
    # 导出符号
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

# Linux特定的版本信息
if(UNIX AND NOT APPLE)
    # 设置RPATH
    set_target_properties(${PROJECT_NAME} PROPERTIES
        INSTALL_RPATH_USE_LINK_PATH TRUE
    )
endif()

# 设置导出符号（Windows DLL）
if(WIN32 AND BUILD_DECODERSDK_SHARED_LIBS)
    target_compile_definitions(${PROJECT_NAME}
        PRIVATE DECODER_SDK_LIB
        INTERFACE DECODER_SDK_DLL
    )
endif()

# 包含目录
target_include_directories(${PROJECT_NAME}
    PRIVATE
        # 内部使用的头文件
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_BINARY_DIR}/../generated
        ${FFMPEG_INCLUDE_DIR}
        ${SPDLOG_INCLUDE_DIR}
        ${JSON_INCLUDE_DIR}
        ${FMT_INCLUDE_DIR}
        ${EVENTPP_INCLUDE_DIR}
        ${MAGIC_ENUM_INCLUDE_DIR}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/../generated>
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

# 引入安装函数
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/DecoderSDKInstall.cmake)

# 设置子项目安装路径
if(NOT DECODER_SDK_IS_MAIN_PROJECT)
    # 允许外部设置安装路径，默认使用 $<CONFIG>
    if(NOT DEFINED DECODER_SDK_INSTALL_DESTINATION)
        set(DECODER_SDK_INSTALL_DESTINATION "$<CONFIG>")
    endif()
endif()

# 安装逻辑
if(DECODER_SDK_IS_MAIN_PROJECT)
    # 主项目安装
    install_decoder_targets(TRUE "")

    # 安装头文件
    install(DIRECTORY include/
        DESTINATION include
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
    )

    # 导出目标
    install(EXPORT DecoderSDKTargets
        FILE DecoderSDKTargets.cmake
        NAMESPACE DecoderSDK::
        DESTINATION lib/cmake/DecoderSDK
    )

    # 生成和安装版本文件
    include(CMakePackageConfigHelpers)
    write_basic_package_version_file(
        "${CMAKE_BINARY_DIR}/DecoderSDKConfigVersion.cmake"
        VERSION "${DECODER_SDK_VERSION_MAJOR}.${DECODER_SDK_VERSION_MINOR}.${DECODER_SDK_VERSION_PATCH}"
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        "${CMAKE_BINARY_DIR}/DecoderSDKConfigVersion.cmake"
        DESTINATION lib/cmake/DecoderSDK
    )
else()
    # 子项目安装
    install_decoder_targets(FALSE "${DECODER_SDK_INSTALL_DESTINATION}")
endif()