# external/CMakeLists.txt

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # Windows-x86_64
    # FFmpeg
    install(DIRECTORY ${FFMPEG_BIN_DIR}/
        DESTINATION $<CONFIG>
        FILES_MATCHING PATTERN "*.dll"
    )
    # spdlog
    install(DIRECTORY ${SPDLOG_BIN_DIR}/
        DESTINATION $<CONFIG>
        FILES_MATCHING PATTERN "*.dll"
    )

elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    

else ()
    # 不支持的系统或处理器架构
    message(FATAL_ERROR "Unsupported system ${CMAKE_SYSTEM_NAME} or unsupported processor ${CMAKE_SYSTEM_PROCESSOR}")
endif ()
