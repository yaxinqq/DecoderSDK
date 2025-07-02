# Qt GUI Example 主工程

# 设置可执行文件名
set(EXAMPLE_TARGET qt_gui_example)

# 使用通配符收集源文件
file(GLOB_RECURSE MAIN_SOURCES
    "*.cpp"
    "*.h"
)

# 使用通配符收集UI文件
file(GLOB_RECURSE UI_FILES
    "*.ui"
)

# 创建可执行文件
add_executable(${EXAMPLE_TARGET}
    ${MAIN_SOURCES}
    ${UI_FILES}
)

# Windows 特定配置
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # 运行时路径配置
    set(QT_BIN_DIR "${Qt5_DIR}/../../../bin")
    set(THIRD_PARTY_LIB_DIRS
        "${FFMPEG_BIN_DIR}"
        "${SPDLOG_BIN_DIR}"
    )
    string(JOIN ";" RUNTIME_PATHS ${QT_BIN_DIR} ${THIRD_PARTY_LIB_DIRS})
    set_target_properties(${EXAMPLE_TARGET} PROPERTIES
        VS_DEBUGGER_ENVIRONMENT "PATH=%PATH%;${RUNTIME_PATHS}"
    )
endif()


# 设置目标属性
set_target_properties(${EXAMPLE_TARGET} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# 包含目录
target_include_directories(${EXAMPLE_TARGET} PRIVATE
    ${GLOBAL_INCLUDE_DIRS}
)

# 链接库
target_link_libraries(${EXAMPLE_TARGET} PRIVATE
    ${GLOBAL_LINK_LIBRARIES}
)

# 编译定义
target_compile_definitions(${EXAMPLE_TARGET} PRIVATE
    QT_MESSAGELOGCONTEXT
)

# 平台特定设置
if(WIN32)
    # Windows特定设置
    set_target_properties(${EXAMPLE_TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/examples/qt_gui/Debug
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/examples/qt_gui/Release
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/examples/qt_gui/RelWithDebInfo
    )
    
    # 控制台窗口配置（仅Windows）
    target_link_options(${EXAMPLE_TARGET} PRIVATE
        $<$<CONFIG:Debug>:/SUBSYSTEM:CONSOLE>
        $<$<CONFIG:Release>:/SUBSYSTEM:CONSOLE>
        $<$<CONFIG:RelWithDebInfo>:/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup>
    )

    # 复制DecoderSDK库文件到输出目录
    add_custom_command(TARGET ${EXAMPLE_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:DecoderSDK>
        $<TARGET_FILE_DIR:${EXAMPLE_TARGET}>
        COMMENT "Copying DecoderSDK DLL to qt_gui example output directory"
    )
    
    # 复制DecoderSDK库PDB到输出目录
    add_custom_command(TARGET ${EXAMPLE_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_PDB_FILE:DecoderSDK>
        $<TARGET_FILE_DIR:${EXAMPLE_TARGET}>
        COMMENT "Copying DecoderSDK PDB to qt_gui example output directory"
    )
elseif(UNIX)
    # Linux/Unix特定设置
    set_target_properties(${EXAMPLE_TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples/qt_gui
    )
    
    # Linux下复制共享库文件
    if(TARGET DecoderSDK)
        add_custom_command(TARGET ${EXAMPLE_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:DecoderSDK>
            $<TARGET_FILE_DIR:${EXAMPLE_TARGET}>
            COMMENT "Copying DecoderSDK shared library to qt_gui example output directory"
        )
    endif()
    
    # 设置RPATH以便找到共享库
    set_target_properties(${EXAMPLE_TARGET} PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()

# 设置Visual Studio筛选器（仅Windows/MSVC）
if(MSVC)
    # 收集所有源文件但分别处理
    file(GLOB_RECURSE ALL_SOURCES "*.cpp" "*.h")
    
    # 分别收集不同文件夹的文件
    file(GLOB ROOT_SOURCES "*.cpp" "*.h")
    file(GLOB VIDEOPLAYER_SOURCES "VideoPlayer/*.cpp" "VideoPlayer/*.h")
    file(GLOB_RECURSE VIDEORENDER_SOURCES "VideoPlayer/VideoRender/*.cpp" "VideoPlayer/VideoRender/*.h")
    
    # 创建筛选器
    source_group("Source Files" FILES ${ROOT_SOURCES})
    source_group("VideoPlayer" FILES ${VIDEOPLAYER_SOURCES})
    source_group("VideoRender" FILES ${VIDEORENDER_SOURCES})
    
    # 为UI文件创建筛选器
    if(UI_FILES)
        source_group("Form Files" FILES ${UI_FILES})
    endif()
endif()