#include "logger.h"

#include <chrono>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <nlohmann/json.hpp>

extern "C" {
#include <libavutil/log.h>
}

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace {
void initDefaultLevelConfig(LogConfig *config)
{
    config->levels["trace"] = LevelConfig{50, 1000, 1};      // 50MB单文件，1000MB总大小，1天保留
    config->levels["debug"] = LevelConfig{100, 1000, 3};     // 100MB单文件，500MB文件夹，3天保留
    config->levels["info"] = LevelConfig{100, 500, 7};       // 100MB单文件，1GB文件夹，7天保留
    config->levels["warning"] = LevelConfig{100, 500, 30};   // 100MB单文件，500MB文件夹，30天保留
    config->levels["error"] = LevelConfig{100, 500, 90};     // 100MB单文件，500MB文件夹，90天保留
    config->levels["critical"] = LevelConfig{100, 500, 180}; // 100MB单文件，500MB文件夹，180天保留
    config->levels["all"] = LevelConfig{100, 1000, 5}; // all日志：100MB单文件，1GB文件夹，5天保留
}

spdlog::level::level_enum parseLevel(const std::string &level)
{
    if (level == "trace")
        return spdlog::level::trace;
    if (level == "debug")
        return spdlog::level::debug;
    if (level == "info")
        return spdlog::level::info;
    if (level == "warn" || level == "warning")
        return spdlog::level::warn;
    if (level == "error")
        return spdlog::level::err;
    if (level == "fatal" || level == "critical")
        return spdlog::level::critical;
    return spdlog::level::info;
}

spdlog::level::level_enum convertFFmpegLevel(int ffmpegLevel)
{
    switch (ffmpegLevel) {
        case AV_LOG_PANIC:
        case AV_LOG_FATAL:
            return spdlog::level::critical;
        case AV_LOG_ERROR:
            return spdlog::level::err;
        case AV_LOG_WARNING:
            return spdlog::level::warn;
        case AV_LOG_INFO:
            return spdlog::level::info;
        case AV_LOG_VERBOSE:
        case AV_LOG_DEBUG:
            return spdlog::level::debug;
        case AV_LOG_TRACE:
            return spdlog::level::trace;
        default:
            return spdlog::level::info;
    }
}

int convertSpdlogLevel(spdlog::level::level_enum spdlogLevel)
{
    switch (spdlogLevel) {
        case spdlog::level::critical:
            return AV_LOG_FATAL;
        case spdlog::level::err:
            return AV_LOG_ERROR;
        case spdlog::level::warn:
            return AV_LOG_WARNING;
        case spdlog::level::info:
            return AV_LOG_INFO;
        case spdlog::level::debug:
            return AV_LOG_DEBUG;
        case spdlog::level::trace:
            return AV_LOG_TRACE;
        default:
            return AV_LOG_INFO;
    }
}

} // namespace

// 静态成员变量定义
std::unique_ptr<LogConfig> LoggerManager::config_ = nullptr;
std::shared_ptr<spdlog::logger> LoggerManager::logger_ = nullptr;
std::mutex LoggerManager::initMutex_;
std::atomic<bool> LoggerManager::initialized_{false};
std::string LoggerManager::defaultConfigPath_ = "etc/decoderSDK.json";

void LoggerManager::ffmpegLogCallback(void *avcl, int level, const char *fmt, va_list vl)
{
    if (level > av_log_get_level())
        return;

    // 格式化FFmpeg日志消息
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, vl);

    // 移除末尾的换行符
    std::string message(buffer);
    if (!message.empty() && message.back() == '\n') {
        message.pop_back();
    }

    // 跳过空消息
    if (message.empty()) {
        return;
    }

    // 添加[FFMPEG]前缀
    std::string prefixedMessage = "[FFMPEG] " + message;

    // 转换日志级别并记录
    spdlog::level::level_enum spdlogLevel = convertFFmpegLevel(level);
    logFFmpeg(spdlogLevel, prefixedMessage);
}

void LoggerManager::logFFmpeg(spdlog::level::level_enum level, const std::string &message)
{
    auto logger = getLogger();
    if (logger && logger->should_log(level)) {
        // 使用特殊的源位置信息标识这是来自FFmpeg的日志
        logger->log(spdlog::source_loc{"ffmpeg", 0, "ffmpeg"}, level, message);
    }
}

void LoggerManager::setupFFmpegLogging()
{
    // 设置FFmpeg日志级别，只记录INFO及以上级别的日志
    av_log_set_level(convertSpdlogLevel(parseLevel(config_->level)));

    // 设置FFmpeg日志回调函数
    av_log_set_callback(ffmpegLogCallback);
}

bool LoggerManager::loadConfig(const std::string &configFile, LogConfig &config)
{
    try {
        std::ifstream file(configFile);
        if (!file.is_open()) {
            std::cerr << "无法打开配置文件: " << configFile << std::endl;
            return false;
        }

        nlohmann::json j;
        file >> j;

        if (!j.contains("log")) {
            std::cerr << "配置文件中缺少log配置节" << std::endl;
            return false;
        }

        const auto logConfig = j["log"];

        // 基本配置
        config.enableFileLog = logConfig.value("enableFileLog", config.enableConsoleLog);
        config.enableConsoleLog = logConfig.value("enableConsoleLog", config.enableConsoleLog);
        config.enableLevelSplit = logConfig.value("enableLevelSplit", config.enableLevelSplit);
        config.logDir = logConfig.value("logDir", config.logDir);
        config.level = logConfig.value("level", config.level);
        config.pattern = logConfig.value("pattern", config.pattern);

        // 分级配置（包括all级别）
        const std::vector<std::string> levels = {"trace", "debug",    "info", "warning",
                                                 "error", "critical", "all"};
        for (const auto &level : levels) {
            if (logConfig.contains("levels") && logConfig["levels"].contains(level)) {
                auto levelJson = logConfig["levels"][level];
                config.levels[level].maxFileSizeMB =
                    levelJson.value("maxFileSizeMB", config.levels[level].maxFileSizeMB);
                config.levels[level].overallFileSizeMB =
                    levelJson.value("overallFileSizeMB", config.levels[level].overallFileSizeMB);
                config.levels[level].retentionDays =
                    levelJson.value("retentionDays", config.levels[level].retentionDays);
            }
        }

        return true;
    } catch (const std::exception &e) {
        std::cerr << "解析配置文件失败: " << e.what() << std::endl;
        return false;
    }
}

std::string LoggerManager::getLogDir()
{
    return config_->logDir + "/DecoderSDK";
}

void LoggerManager::createLogger()
{
    if (!config_) {
        return;
    }

    try {
        // 初始化异步线程池
        spdlog::init_thread_pool(8192, 1);

        std::vector<spdlog::sink_ptr> sinks;

        // 获取配置的日志级别
        const spdlog::level::level_enum configLevel = parseLevel(config_->level);

        // 控制台输出sink
        if (config_->enableConsoleLog) {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            console_sink->set_level(configLevel);
            sinks.push_back(console_sink);
        }

        // 文件输出sinks
        if (config_->enableFileLog) {
            // 创建日志目录，如果不输出到文件。则不创建
            const auto logDir = getLogDir();
            std::filesystem::create_directories(logDir);
            if (config_->enableLevelSplit) {
                // 开启分级输出：为每个级别创建独立的文件sink
                const std::vector<std::pair<std::string, spdlog::level::level_enum>> allLevels = {
                    {"trace", spdlog::level::trace}, {"debug", spdlog::level::debug},
                    {"info", spdlog::level::info},   {"warning", spdlog::level::warn},
                    {"error", spdlog::level::err},   {"critical", spdlog::level::critical}};

                // 为配置级别及以上的级别创建独立的文件sink
                for (const auto &[levelName, levelEnum] : allLevels) {
                    // 只创建配置级别及以上级别的sink
                    if (levelEnum >= configLevel) {
                        auto it = config_->levels.find(levelName);
                        if (it != config_->levels.end()) {
                            const auto &levelConfig = it->second;

                            const size_t maxSize =
                                static_cast<size_t>(levelConfig.maxFileSizeMB) * 1024 * 1024;
                            const bool hourlyRotation =
                                (levelName == "trace"); // trace级别按小时轮转

                            // 创建只接收特定级别的sink
                            auto file_sink = std::make_shared<LoggerSinkMutex>(
                                logDir, maxSize, levelConfig.overallFileSizeMB, hourlyRotation,
                                levelConfig.retentionDays, levelEnum, false);

                            file_sink->set_level(levelEnum);
                            sinks.push_back(file_sink);
                        }
                    }
                }
            }

            // 创建all sink（聚合日志，但同样受level限制）
            auto allIt = config_->levels.find("all");
            if (allIt != config_->levels.end()) {
                const auto &allConfig = allIt->second;

                const size_t allMaxSize =
                    static_cast<size_t>(allConfig.maxFileSizeMB) * 1024 * 1024;

                // all sink接收配置级别及以上的所有日志
                auto all_file_sink = std::make_shared<LoggerSinkMutex>(
                    logDir, allMaxSize, allConfig.overallFileSizeMB, false, allConfig.retentionDays,
                    configLevel, true);

                all_file_sink->set_level(spdlog::level::trace); // 让sink内部处理级别过滤
                sinks.push_back(all_file_sink);
            }
        }

        if (!sinks.empty()) {
            logger_ = std::make_shared<spdlog::async_logger>("DecoderSDK", sinks.begin(),
                                                             sinks.end(), spdlog::thread_pool(),
                                                             spdlog::async_overflow_policy::block);

            // 设置logger的级别为配置级别
            logger_->set_level(configLevel);
            // 配置格式
            logger_->set_pattern(config_->pattern);
            // 配置刷新方式
            logger_->flush_on(spdlog::level::warn);
            spdlog::flush_every(std::chrono::seconds(1));

            // 注册日志器
            spdlog::register_logger(logger_);
        }
    } catch (const std::exception &e) {
        std::cerr << "创建日志器失败: " << e.what() << std::endl;
    }
}

bool LoggerManager::initialize(const std::string &configFile)
{
    std::lock_guard<std::mutex> lock(initMutex_);

    if (initialized_.load()) {
        return true;
    }

    config_ = std::make_unique<LogConfig>();
    const std::string configPath = configFile.empty() ? defaultConfigPath_ : configFile;

    // 设置各个级别的默认配置
    initDefaultLevelConfig(config_.get());

    // 读取配置，对默认配置进行增量更新
    loadConfig(configPath, *config_);

    // 创建日志器
    createLogger();

    // 设置FFmpeg日志回调
    setupFFmpegLogging();

    initialized_.store(true);
    return true;
}

std::shared_ptr<spdlog::logger> LoggerManager::getLogger()
{
    if (!initialized_.load()) {
        initialize();
    }
    return logger_;
}

bool LoggerManager::reloadConfig(const std::string &configFile)
{
    std::lock_guard<std::mutex> lock(initMutex_);

    initialized_.store(false);

    LogConfig newConfig;
    const std::string configPath = configFile.empty() ? defaultConfigPath_ : configFile;
    initDefaultLevelConfig(&newConfig);
    loadConfig(configPath, newConfig);

    // 关闭现有日志器
    logger_ = nullptr;
    spdlog::drop_all();

    // 应用新配置
    *config_ = std::move(newConfig);
    createLogger();

    // 重新设置FFmpeg日志回调
    setupFFmpegLogging();

    initialized_.store(true);

    return true;
}

void LoggerManager::shutdown()
{
    std::lock_guard<std::mutex> lock(initMutex_);

    if (!initialized_.load()) {
        return;
    }

    // 恢复FFmpeg默认日志处理
    av_log_set_callback(av_log_default_callback);

    logger_->flush();
    spdlog::drop_all();
    spdlog::shutdown();

    // 清理日志器
    logger_ = nullptr;

    initialized_.store(false);
}

std::string LoggerManager::getLogStats()
{
    if (!initialized_.load()) {
        return "日志系统未初始化";
    }

    std::stringstream ss;
    ss << "日志系统状态:\n";
    ss << "- 配置文件: " << defaultConfigPath_ << "\n";
    ss << "- 日志目录: " << (config_ ? config_->logDir : "未知") << "\n";
    ss << "- 当前级别: " << (config_ ? config_->level : "未知") << "\n";
    ss << "- 文件日志: " << (config_ && config_->enableFileLog ? "启用" : "禁用") << "\n";
    ss << "- 控制台日志: " << (config_ && config_->enableConsoleLog ? "启用" : "禁用") << "\n";
    ss << "- FFmpeg日志: 已集成\n";

    return ss.str();
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END