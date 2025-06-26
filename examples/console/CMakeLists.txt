# Decoder Usage Example CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

project(decoder_usage_example)

# 设置可执行文件名
set(EXAMPLE_TARGET decoder_usage_example)

# 源文件
set(EXAMPLE_SOURCES
    decoder_usage_example.cpp
)

# 创建可执行文件
add_executable(${EXAMPLE_TARGET} ${EXAMPLE_SOURCES})

# 设置C++标准
set_target_properties(${EXAMPLE_TARGET} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# 包含目录
target_include_directories(${EXAMPLE_TARGET} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_SOURCE_DIR}/src
    ${FFMPEG_INCLUDE_DIR}
    ${SPDLOG_INCLUDE_DIR}
    ${FMT_INCLUDE_DIR}
    ${EVENTPP_INCLUDE_DIR}
    ${MAGIC_ENUM_INCLUDE_DIR}
    ${JSON_INCLUDE_DIR}
)

# 链接库
target_link_libraries(${EXAMPLE_TARGET} PRIVATE
    DecoderSDK  # 主库
)

# Windows特定设置
if(WIN32)
    # 设置输出目录
    set_target_properties(${EXAMPLE_TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/examples/Debug
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/examples/Release
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/examples/RelWithDebInfo
    )
    
    # 复制必要的DLL文件到输出目录
    add_custom_command(TARGET ${EXAMPLE_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${FFMPEG_BIN_DIR}
        $<TARGET_FILE_DIR:${EXAMPLE_TARGET}>
        COMMENT "Copying FFmpeg DLLs to example output directory"
    )
    
    add_custom_command(TARGET ${EXAMPLE_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${SPDLOG_BIN_DIR}
        $<TARGET_FILE_DIR:${EXAMPLE_TARGET}>
        COMMENT "Copying spdlog DLLs to example output directory"
    )

    # 复制DecoderSDK库DLL到输出目录
    add_custom_command(TARGET ${EXAMPLE_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:DecoderSDK>
        $<TARGET_FILE_DIR:${EXAMPLE_TARGET}>
        COMMENT "Copying DecoderSDK DLL to example output directory"
    )

    # 复制DecoderSDK库PDB到输出目录
    add_custom_command(TARGET ${EXAMPLE_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_PDB_FILE:DecoderSDK>
        $<TARGET_FILE_DIR:${EXAMPLE_TARGET}>
        COMMENT "Copying DecoderSDK PDB to example output directory"
    )
endif()

# 创建images输出目录
add_custom_command(TARGET ${EXAMPLE_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
    $<TARGET_FILE_DIR:${EXAMPLE_TARGET}>/images
    COMMENT "Creating images directory for example output"
)

# 复制配置文件
add_custom_command(TARGET ${EXAMPLE_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/resources/decoderSDK.json
    $<TARGET_FILE_DIR:${EXAMPLE_TARGET}>/etc/decoderSDK.json
    COMMENT "Copying configuration file to example output directory"
)