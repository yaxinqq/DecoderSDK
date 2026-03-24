cmake_minimum_required(VERSION 3.16)

project(encode_demo)

set(TARGET_NAME "encode_demo")

find_package(Vulkan REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EXTERNAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external)

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)

set(GLFW_INCLUDE_DIR ${EXTERNAL_DIR}/GLFW/include)
set(GLFW_LIBRARY ${EXTERNAL_DIR}/GLFW/lib/glfw3.lib)

add_library(vk_bootstrap STATIC
    ${EXTERNAL_DIR}/vk_bootstrap/include/VkBootstrap.cpp
    ${EXTERNAL_DIR}/vk_bootstrap/include/VkBootstrap.h
    ${EXTERNAL_DIR}/vk_bootstrap/include/VkBootstrapDispatch.h
    ${EXTERNAL_DIR}/vk_bootstrap/include/VkBootstrapFeatureChain.h
    ${EXTERNAL_DIR}/vk_bootstrap/include/VkBootstrapFeatureChain.inl
)

target_include_directories(vk_bootstrap PUBLIC
    ${EXTERNAL_DIR}/vk_bootstrap/include
    ${Vulkan_INCLUDE_DIRS}
)
target_link_libraries(vk_bootstrap PUBLIC ${Vulkan_LIBRARIES})

add_executable(${TARGET_NAME}
    src/main.cpp
    src/VulkanOsdDemoApp.cpp
    src/VulkanOsdDemoApp.h
    src/VideoDecoder.cpp
    src/VideoDecoder.h
    src/CudaContext.cpp
    src/CudaContext.h
    src/VulkanRenderer.cpp
    src/VulkanRenderer.h
    src/OsdTextRendererStb.cpp
    src/OsdTextRendererStb.h
    src/vma_implementation.cpp
    src/stb_truetype.h
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${EXTERNAL_DIR}/vk_bootstrap/include
    ${EXTERNAL_DIR}/vma/include
    ${EXTERNAL_DIR}/shaderc/include
    ${GLFW_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/include
)

set(SHADERC_INCLUDE_DIR ${EXTERNAL_DIR}/shaderc/include)
set(SHADERC_SHARED_LIB ${EXTERNAL_DIR}/shaderc/lib/shaderc_shared.lib)
set(SHADERC_SHARED_DLL ${EXTERNAL_DIR}/shaderc/bin/shaderc_shared.dll)

if(NOT EXISTS "${SHADERC_SHARED_LIB}")
    message(FATAL_ERROR "shaderc_shared.lib not found: ${SHADERC_SHARED_LIB}")
endif()

target_link_libraries(${TARGET_NAME} PRIVATE
    DecoderSDK
    vk_bootstrap
    ${GLFW_LIBRARY}
    ${Vulkan_LIBRARIES}
    ${SHADERC_SHARED_LIB}
)

if(CUDA_FOUND)
    target_include_directories(${TARGET_NAME} PRIVATE ${CUDA_INCLUDE_DIR})
    if(WIN32)
        target_link_directories(${TARGET_NAME} PRIVATE ${CUDA_LIB_DIR})
        target_link_libraries(${TARGET_NAME} PRIVATE cuda)
    else()
        if(CUDAToolkit_FOUND)
            target_link_libraries(${TARGET_NAME} PRIVATE CUDA::cuda_driver)
        else()
            target_link_libraries(${TARGET_NAME} PRIVATE ${CUDA_LIBRARIES})
        endif()
    endif()
endif()

if(WIN32)
    target_link_libraries(${TARGET_NAME} PRIVATE user32 gdi32 shell32)
    target_compile_definitions(${TARGET_NAME} PRIVATE
        GLFW_INCLUDE_VULKAN
        VK_USE_PLATFORM_WIN32_KHR
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    if(FFMPEG_BIN_DIR AND EXISTS "${FFMPEG_BIN_DIR}")
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${FFMPEG_BIN_DIR}
            $<TARGET_FILE_DIR:${TARGET_NAME}>
            COMMENT "Copying FFmpeg DLLs to demo output directory"
        )
    endif()

    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${SHADER_DIR}
        $<TARGET_FILE_DIR:${TARGET_NAME}>/shaders
        COMMENT "Copying shader sources to demo output directory"
    )

    if(EXISTS "${SHADERC_SHARED_DLL}")
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SHADERC_SHARED_DLL}
            $<TARGET_FILE_DIR:${TARGET_NAME}>
            COMMENT "Copying shaderc_shared.dll to demo output directory"
        )
    else()
        message(WARNING "shaderc_shared.dll not found: ${SHADERC_SHARED_DLL}. Demo may fail to start.")
    endif()

    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:DecoderSDK>
        $<TARGET_FILE_DIR:${TARGET_NAME}>
        COMMENT "Copying DecoderSDK DLL to demo output directory"
    )

    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_PDB_FILE:DecoderSDK>
        $<TARGET_FILE_DIR:${TARGET_NAME}>
        COMMENT "Copying DecoderSDK PDB to demo output directory"
    )
endif()
