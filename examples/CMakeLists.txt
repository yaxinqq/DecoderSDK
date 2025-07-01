# Examples CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

# 设置示例项目的通用配置
set(EXAMPLES_OUTPUT_DIR ${CMAKE_BINARY_DIR}/examples)

# 创建示例输出目录
file(MAKE_DIRECTORY ${EXAMPLES_OUTPUT_DIR})

# 根据选项添加示例子目录
if(BUILD_CONSOLE_EXAMPLE)
    message(STATUS "Building console example")
    add_subdirectory(console)
else()
    message(STATUS "Skipping console example")
endif()

if(BUILD_QT_GUI_EXAMPLE)
    message(STATUS "Building Qt GUI example")
    add_subdirectory(qt_gui)
else()
    message(STATUS "Skipping Qt GUI example")
endif()

if(BUILD_D3D_RENDER_EXAMPLE)
    message(STATUS "Building D3D render example")
    add_subdirectory(d3d_render_example)
else()
    message(STATUS "Skipping D3D render example")
endif()
