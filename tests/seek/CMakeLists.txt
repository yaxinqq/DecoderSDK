# tests/seek/CMakeLists.txt

add_executable(seek_coordinator_test 
    seek_coordinator_test.cpp
    ${CMAKE_SOURCE_DIR}/src/base/seek_coordinator.cpp
)

target_link_libraries(seek_coordinator_test PRIVATE DecoderSDK)

# 设置包含目录以便访问 internal 头文件
set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/include
    ${FFMPEG_INCLUDE_DIR}
    ${VULKAN_INCLUDE_DIR}
    ${EVENTPP_INCLUDE_DIR}
)

target_include_directories(seek_coordinator_test PRIVATE ${TEST_INCLUDE_DIRS})

add_test(NAME seek_coordinator_test COMMAND seek_coordinator_test)
