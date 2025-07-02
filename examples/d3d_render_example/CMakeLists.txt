cmake_minimum_required(VERSION 3.16)

project(d3d_render_example)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置源文件
set(SOURCES
    main.cpp
    D3DRenderer.cpp
    D3DRenderer.h
)

# 创建可执行文件
add_executable(${PROJECT_NAME} ${SOURCES})

# 链接库
target_link_libraries(${PROJECT_NAME}
    DecoderSDK
    d3d11
    dxgi
    d3dcompiler
)

# 包含目录
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/include
)

# Windows特定设置
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/examples/d3d_render_example/Debug)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/examples/d3d_render_example/Release)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/examples/d3d_render_example/RelWithDebInfo)

# 平台特定设置
if(WIN32)
    # Windows特定设置
    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/examples/d3d_render_example/Debug
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/examples/d3d_render_example/Release
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/examples/d3d_render_example/RelWithDebInfo
    )
    
    # 复制DecoderSDK库文件到输出目录
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:DecoderSDK>
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMENT "Copying DecoderSDK DLL to d3d_render_example output directory"
    )
    
    # 复制DecoderSDK库PDB到输出目录
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_PDB_FILE:DecoderSDK>
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMENT "Copying DecoderSDK PDB to d3d_render_example output directory"
    )
elseif(UNIX)
    # Linux/Unix特定设置
    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples/d3d_render_example
    )
    
    # Linux下复制共享库文件
    if(TARGET DecoderSDK)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:DecoderSDK>
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
            COMMENT "Copying DecoderSDK shared library to d3d_render_example output directory"
        )
    endif()
    
    # 设置RPATH以便找到共享库
    set_target_properties(${PROJECT_NAME} PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()