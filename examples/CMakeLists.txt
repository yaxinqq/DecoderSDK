# Examples CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

# 设置示例项目的通用配置
set(EXAMPLES_OUTPUT_DIR ${CMAKE_BINARY_DIR}/examples)

# 创建示例输出目录
file(MAKE_DIRECTORY ${EXAMPLES_OUTPUT_DIR})

# 添加示例子目录
add_subdirectory(console)

# 可以在这里添加更多示例项目
# add_subdirectory(other_example)