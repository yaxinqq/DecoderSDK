#ifndef DECODER_SDK_INTERNAL_LOGGER_H
#define DECODER_SDK_INTERNAL_LOGGER_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/common.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "base/base_define.h"
#include "logger_sink.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

// 日志级别配置结构
struct LevelConfig {
    // 单个日志文件大小
    int maxFileSizeMB = 100;
    // 此级别下，所有日志文件的大小
    int overallFileSizeMB = 500;
    // 最多保存天数
    int retentionDays = 30;
};

// 主日志配置结构
struct LogConfig {
    // 是否开启文件输出，默认不开启
    bool enableFileLog = false;
    // 是否开启命令行输出，开启
    bool enableConsoleLog = true;
    // 是否开启分级输出，默认不开启
    bool enableLevelSplit = false;
    // 日志存放的根目录
    std::string logDir = "./logs";
    // 输出日志级别
    std::string level = "info";
    // 日志格式
    std::string pattern = "[%Y-%m-%d %H:%M:%S.%e] [%t] [%^%l%$] [%s:%#] %v";

    // 分级配置（包括all级别）
    std::unordered_map<std::string, LevelConfig> levels;
};

class LoggerManager {
public:
    /**
     * @brief 初始化日志系统
     * @param configFile 配置文件路径
     * @return 是否初始化成功
     */
    static bool initialize(const std::string &configFile = "");

    /**
     * @brief 获取日志器
     * @return 日志器指针
     */
    static std::shared_ptr<spdlog::logger> getLogger();

    /**
     * @brief 重新加载配置
     * @param configFile 配置文件路径
     */
    static bool reloadConfig(const std::string &configFile = "");

    /**
     * @brief 关闭日志系统
     */
    static void shutdown();

    /**
     * @brief 获取日志统计信息
     */
    static std::string getLogStats();

    /**
     * @brief 设置FFmpeg日志回调
     */
    static void setupFFmpegLogging();

    /**
     * @brief FFmpeg日志回调函数
     * @param avcl FFmpeg上下文
     * @param level FFmpeg日志级别
     * @param fmt 格式字符串
     * @param vl 参数列表
     */
    static void ffmpegLogCallback(void* avcl, int level, const char* fmt, va_list vl);

    /**
     * @brief 通用日志记录函数
     * @param level 日志级别
     * @param file 文件名
     * @param line 行号
     * @param func 函数名
     * @param format 格式字符串
     * @param args 参数
     */
    template <typename... Args>
    static void log(spdlog::level::level_enum level, const char *file, int line, const char *func,
                    const char *format, Args &&...args)
    {
        auto logger = getLogger();
        if (logger && logger->should_log(level)) {
            logger->log(spdlog::source_loc{file, line, func}, level, format,
                        std::forward<Args>(args)...);
        }
    }

    /**
     * @brief FFmpeg专用日志记录函数
     * @param level 日志级别
     * @param message 日志消息
     */
    static void logFFmpeg(spdlog::level::level_enum level, const std::string& message);

private:
    static bool loadConfig(const std::string &configFile, LogConfig &config);
    static void createLogger();
    static std::string getLogDir();

private:
    static std::unique_ptr<LogConfig> config_;
    static std::shared_ptr<spdlog::logger> logger_;
    static std::mutex initMutex_;
    static std::atomic<bool> initialized_;
    static std::string defaultConfigPath_;
};

// 简化的日志宏定义
#define LOG_TRACE(...) \
    LoggerManager::log(spdlog::level::trace, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_DEBUG(...) \
    LoggerManager::log(spdlog::level::debug, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_INFO(...) \
    LoggerManager::log(spdlog::level::info, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_WARN(...) \
    LoggerManager::log(spdlog::level::warn, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_ERROR(...) \
    LoggerManager::log(spdlog::level::err, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_FATAL(...) \
    LoggerManager::log(spdlog::level::critical, __FILE__, __LINE__, __func__, __VA_ARGS__)

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_LOGGER_H