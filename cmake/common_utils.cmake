function(detect_ffmpeg_version)
    if (NOT FFMPEG_INCLUDE_DIR)
        message(FATAL_ERROR
            "detect_ffmpeg_version: FFMPEG_INCLUDE_DIR is not set")
    endif()

    set(_avutil_version_h
        "${FFMPEG_INCLUDE_DIR}/libavutil/version.h")
    message(STATUS "FFmpeg avutil version header: ${_avutil_version_h}")

    if (NOT EXISTS "${_avutil_version_h}")
        message(FATAL_ERROR
            "FFmpeg header not found: ${_avutil_version_h}")
    endif()

    file(READ "${_avutil_version_h}" _avutil_version_content)

    string(REGEX MATCH
        "#define[ \t]+LIBAVUTIL_VERSION_MAJOR[ \t]+([0-9]+)"
        _major_match
        "${_avutil_version_content}")
    set(FFMPEG_AVUTIL_VERSION_MAJOR "${CMAKE_MATCH_1}")

    string(REGEX MATCH
        "#define[ \t]+LIBAVUTIL_VERSION_MINOR[ \t]+([0-9]+)"
        _minor_match
        "${_avutil_version_content}")
    set(FFMPEG_AVUTIL_VERSION_MINOR "${CMAKE_MATCH_1}")

    string(REGEX MATCH
        "#define[ \t]+LIBAVUTIL_VERSION_MICRO[ \t]+([0-9]+)"
        _micro_match
        "${_avutil_version_content}")
    set(FFMPEG_AVUTIL_VERSION_MICRO "${CMAKE_MATCH_1}")

    if (FFMPEG_AVUTIL_VERSION_MAJOR STREQUAL "")
        message(FATAL_ERROR "Failed to parse FFmpeg version")
    endif()

    set(FFMPEG_AVUTIL_VERSION_STRING
        "${FFMPEG_AVUTIL_VERSION_MAJOR}.${FFMPEG_AVUTIL_VERSION_MINOR}.${FFMPEG_AVUTIL_VERSION_MICRO}")

    # 向父作用域导出
    set(FFMPEG_AVUTIL_VERSION_MAJOR
        ${FFMPEG_AVUTIL_VERSION_MAJOR} PARENT_SCOPE)
    set(FFMPEG_AVUTIL_VERSION_MINOR
        ${FFMPEG_AVUTIL_VERSION_MINOR} PARENT_SCOPE)
    set(FFMPEG_AVUTIL_VERSION_MICRO
        ${FFMPEG_AVUTIL_VERSION_MICRO} PARENT_SCOPE)
    set(FFMPEG_AVUTIL_VERSION_STRING
        ${FFMPEG_AVUTIL_VERSION_STRING} PARENT_SCOPE)

    message(STATUS
        "Detected FFmpeg avutil version: "
        "${FFMPEG_AVUTIL_VERSION_STRING}")
endfunction()