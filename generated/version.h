#ifndef DECODER_SDK_INTERNAL_VERSION_H
#define DECODER_SDK_INTERNAL_VERSION_H

#include "base_define.h"

/**
 * @file version.h
 * @brief DecoderSDK版本信息定义
 */

// SDK版本信息
#define DECODER_SDK_VERSION_MAJOR @DECODER_SDK_VERSION_MAJOR @
#define DECODER_SDK_VERSION_MINOR @DECODER_SDK_VERSION_MINOR @
#define DECODER_SDK_VERSION_PATCH @DECODER_SDK_VERSION_PATCH @
#define DECODER_SDK_VERSION_BUILD @DECODER_SDK_VERSION_BUILD @

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

// 辅助宏：将数字转换为字符串
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

// 版本字符串 - 自动从版本号宏拼接生成
#define DECODER_SDK_VERSION_STRING      \
    TOSTRING(DECODER_SDK_VERSION_MAJOR) \
    "." TOSTRING(DECODER_SDK_VERSION_MINOR) "." TOSTRING(DECODER_SDK_VERSION_PATCH)
#define DECODER_SDK_VERSION_STRING_FULL                                                           \
    TOSTRING(DECODER_SDK_VERSION_MAJOR)                                                           \
    "." TOSTRING(DECODER_SDK_VERSION_MINOR) "." TOSTRING(DECODER_SDK_VERSION_PATCH) "." TOSTRING( \
        DECODER_SDK_VERSION_BUILD)

// 构建信息
#define DECODER_SDK_BUILD_DATE __DATE__
#define DECODER_SDK_BUILD_TIME __TIME__
#define DECODER_SDK_GIT_HASH "488f401"

// 编译器信息
#ifdef _MSC_VER
#define DECODER_SDK_COMPILER "MSVC"
#define DECODER_SDK_COMPILER_VERSION _MSC_VER
#elif defined(__GNUC__)
#define DECODER_SDK_COMPILER "GCC"
#define DECODER_SDK_COMPILER_VERSION (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#elif defined(__clang__)
#define DECODER_SDK_COMPILER "Clang"
#define DECODER_SDK_COMPILER_VERSION \
    (__clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__)
#else
#define DECODER_SDK_COMPILER "Unknown"
#define DECODER_SDK_COMPILER_VERSION 0
#endif

// 平台信息
#ifdef _WIN32
#ifdef _WIN64
#define DECODER_SDK_PLATFORM "Windows x64"
#else
#define DECODER_SDK_PLATFORM "Windows x86"
#endif
#elif defined(__linux__)
#ifdef __x86_64__
#define DECODER_SDK_PLATFORM "Linux x64"
#else
#define DECODER_SDK_PLATFORM "Linux x86"
#endif
#elif defined(__APPLE__)
#ifdef __x86_64__
#define DECODER_SDK_PLATFORM "macOS x64"
#elif defined(__aarch64__)
#define DECODER_SDK_PLATFORM "macOS ARM64"
#else
#define DECODER_SDK_PLATFORM "macOS"
#endif
#else
#define DECODER_SDK_PLATFORM "Unknown"
#endif

// 构建配置
#ifdef _DEBUG
#define DECODER_SDK_BUILD_CONFIG "Debug"
#else
#define DECODER_SDK_BUILD_CONFIG "Release"
#endif

// 版本比较宏
#define DECODER_SDK_VERSION_CHECK(major, minor, patch)                                \
    ((DECODER_SDK_VERSION_MAJOR > (major)) ||                                         \
     (DECODER_SDK_VERSION_MAJOR == (major) && DECODER_SDK_VERSION_MINOR > (minor)) || \
     (DECODER_SDK_VERSION_MAJOR == (major) && DECODER_SDK_VERSION_MINOR == (minor) && \
      DECODER_SDK_VERSION_PATCH >= (patch)))

// 版本号计算宏
#define DECODER_SDK_VERSION_NUMBER                                         \
    (DECODER_SDK_VERSION_MAJOR * 10000 + DECODER_SDK_VERSION_MINOR * 100 + \
     DECODER_SDK_VERSION_PATCH)

/**
 * @brief 获取SDK版本字符串
 * @return 版本字符串，格式为"major.minor.patch"
 */
const char *getVersionString();

/**
 * @brief 获取SDK完整版本字符串
 * @return 完整版本字符串，格式为"major.minor.patch.build"
 */
const char *getVersionStringFull();

/**
 * @brief 获取SDK构建信息
 * @return 构建信息字符串
 */
const char *getBuildInfo();

/**
 * @brief 获取SDK版本号
 * @param major 主版本号输出
 * @param minor 次版本号输出
 * @param patch 补丁版本号输出
 * @param build 构建版本号输出
 */
void getVersion(int *major, int *minor, int *patch, int *build);

/**
 * @brief 检查版本兼容性
 * @param major 要求的主版本号
 * @param minor 要求的次版本号
 * @param patch 要求的补丁版本号
 * @return 如果当前版本兼容返回1，否则返回0
 */
int checkVersion(int major, int minor, int patch);

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_VERSION_H
