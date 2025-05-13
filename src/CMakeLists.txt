# src/ZH_VideoPlayer/CMakeLists.txt

# 定义工程名称变量
set(PROJECT_NAME DecoderSDK)

# 定义源文件列表
set(SRC_FILES
    AudioDecoder.h
    AudioDecoder.cpp
    Clock.h
    Clock.cpp
    DecoderBase.h
    DecoderBase.cpp
    DecoderManager.h
    DecoderManager.cpp
    Demuxer.h
    Demuxer.cpp
    FrameQueue.h
    FrameQueue.cpp
    HardwareAccel.h
    HardwareAccel.cpp
    main.cpp
    PacketQueue.h
    PacketQueue.cpp
    SyncController.h
    SyncController.cpp
    VideoDecoder.h
    VideoDecoder.cpp
)

# 定义库
# add_library(${PROJECT_NAME} SHARED
#     ${SRC_FILES}
# )
add_executable(${PROJECT_NAME}
    ${SRC_FILES}
)

# # 生成资源文件，并参与编译
# if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
#     set(RC_FILE_PATH "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.rc")

#     # 使用 add_custom_command 来执行 generate_version_info.cmake
#     add_custom_command(
#         OUTPUT ${RC_FILE_PATH}
#         COMMAND ${CMAKE_COMMAND} -DORIGIN_SOURCE_DIR=${CMAKE_SOURCE_DIR}
#                                 -DVERSION_FILE_PATH=${VERSION_FILE_PATH}
#                                 -DOUTPUT_RC_FILE=${RC_FILE_PATH}
#                                 -DFILE_DESCRIPTION=${PROJECT_NAME}
#                                 -DINTERNAL_NAME=${PROJECT_NAME}
#                                 -DORIGINAL_FILENAME="${PROJECT_NAME}.dll"
#                                 -DICON_FILE_PATH=""
#                                 -P ${CMAKE_SOURCE_DIR}/cmake/generate_rc.cmake
#         COMMENT "Generating version info .rc file"
#     )

#     # 创建目标确保 .rc 文件被生成
#     add_custom_target(generate_videoplayer_rc ALL DEPENDS ${RC_FILE_PATH})

#     # 让可执行文件依赖于生成的 .rc 文件
#     add_dependencies(${PROJECT_NAME} generate_videoplayer_rc)

#     # 将生成的 .rc 文件添加到编译源文件中
#     target_sources(${PROJECT_NAME} PRIVATE ${RC_FILE_PATH})
# endif()

# 包含目录
target_include_directories(${PROJECT_NAME}
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/external
    ${FFMPEG_INCLUDE_DIR}
    ${SPDLOG_INCLUDE_DIR}
    ${JSON_INCLUDE_DIR}
)

# 添加库文件路径
target_link_directories(${PROJECT_NAME}
    PRIVATE
    ${FFMPEG_LIB_DIR}
    ${SPDLOG_LIB_DIR}
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

# 链接库
target_link_libraries(${PROJECT_NAME}
    PRIVATE
    ${FFMPEG_LIB}
    ${SPDLOG_LIB}
)

# 编译定义
target_compile_definitions(${PROJECT_NAME}
    PRIVATE
    DECODER_SDK_LIB
)

# 定义编译结束后执行的命令
# if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
#     # Windows-x86_64
#     # 删除生成的rc文件
#     add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD COMMAND ${CMAKE_COMMAND} -E remove ${RC_FILE_PATH} COMMENT "Deleting generated ${RC_FILE_PATH}...")
# elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
#     # Kylin-aarch64
# endif ()

# 安装
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION $<CONFIG>
    LIBRARY DESTINATION $<CONFIG>
)
# 安装 PDB 文件
if (MSVC)
    install(FILES "$<TARGET_PDB_FILE:${PROJECT_NAME}>"
        DESTINATION $<CONFIG>
        CONFIGURATIONS Debug RelWithDebInfo Release# 仅在需要的配置下安装
    )
endif ()
