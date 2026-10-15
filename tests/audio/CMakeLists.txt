add_executable(audio_converter_test
    audio_converter_test.cpp
    ${CMAKE_SOURCE_DIR}/src/base/frame.cpp
)

target_link_libraries(audio_converter_test PRIVATE
    DecoderSDK
    avutil
)

target_link_directories(audio_converter_test PRIVATE
    ${FFMPEG_LIB_DIR}
)

target_include_directories(audio_converter_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/include
    ${FFMPEG_INCLUDE_DIR}
    ${VULKAN_INCLUDE_DIR}
    ${EVENTPP_INCLUDE_DIR}
)

add_test(NAME audio_converter_test COMMAND audio_converter_test)

if(WIN32)
    file(GLOB FFMPEG_RUNTIME_DLLS "${FFMPEG_BIN_DIR}/*.dll")
    add_custom_command(TARGET audio_converter_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FFMPEG_RUNTIME_DLLS}
            "$<TARGET_FILE_DIR:audio_converter_test>"
    )
endif()
