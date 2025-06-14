#include "logger.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "nlohmann/json.hpp"
#include "spdlog/async.h"
#include "spdlog/async_logger.h"
#include "spdlog/pattern_formatter.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

namespace {
std::string getTimestampedLogFile(const std::string &dir)
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << dir << "/" << "DecoderSDK_" << std::put_time(&tm, "%Y%m%d%H%M%S") << ".log";
    return oss.str();
}

spdlog::level::level_enum parseLevel(const std::string &level)
{
    std::string l = level;
    std::transform(l.begin(), l.end(), l.begin(), ::tolower);
    if (l == "trace")
        return spdlog::level::trace;
    if (l == "debug")
        return spdlog::level::debug;
    if (l == "info")
        return spdlog::level::info;
    if (l == "warn")
        return spdlog::level::warn;
    if (l == "error")
        return spdlog::level::err;
    if (l == "critical")
        return spdlog::level::critical;
    return spdlog::level::info;
}

struct LogConfig {
    bool enableFileLog = true;
    std::string level = "info";
    std::string logDir = "./logs";
    int maxFileSizeMB = 50;
    int maxFiles = 100;
};

bool loadLogConfigFromJson(const std::string &path, LogConfig &config)
{
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }

    try {
        nlohmann::json j;
        in >> j;
        if (j.contains("log")) {
            auto log = j["log"];
            if (log.contains("enableFileLog"))
                config.enableFileLog = log["enableFileLog"];
            if (log.contains("level"))
                config.level = log["level"];
            if (log.contains("logDir"))
                config.logDir = log["logDir"];
            if (log.contains("maxFileSizeMB"))
                config.maxFileSizeMB = log["maxFileSizeMB"];
            if (log.contains("maxFiles"))
                config.maxFiles = log["maxFiles"];
        }
        return true;
    } catch (std::exception &) {
        return false;
    }
}

void initLoggerWithConfig(const LogConfig &cfg, std::shared_ptr<spdlog::logger> &logger)
{
    try {
        spdlog::init_thread_pool(8192, 1);
        std::vector<spdlog::sink_ptr> sinks;

        // 控制台 sink
        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        consoleSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        sinks.push_back(consoleSink);

        if (cfg.enableFileLog) {
            std::filesystem::create_directories(cfg.logDir);
            std::string logFile = getTimestampedLogFile(cfg.logDir);
            auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                logFile, cfg.maxFileSizeMB * 1024 * 1024, cfg.maxFiles);
            fileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
            sinks.push_back(fileSink);
        }

        const auto lvl = parseLevel(cfg.level);

        logger = std::make_shared<spdlog::async_logger>("logger", sinks.begin(), sinks.end(),
                                                        spdlog::thread_pool(),
                                                        spdlog::async_overflow_policy::block);

        spdlog::register_logger(logger);
        logger->set_level(lvl);
        logger->flush_on(spdlog::level::warn);
    } catch (const std::exception &e) {
        std::cerr << "Logger 初始化失败: " << e.what() << std::endl;
    }
}
} // namespace

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

std::shared_ptr<spdlog::logger> Logger::logger_ = nullptr;
bool Logger::initialized_ = false;
std::mutex Logger::initMutex_;
std::string Logger::defaultConfigPath_ = "./etc/decoderSDK.json";

void Logger::initFromConfig(const std::string &configPath)
{
    std::lock_guard<std::mutex> lock(initMutex_);

    LogConfig cfg;
    bool loaded = loadLogConfigFromJson(configPath, cfg);

    if (!loaded) {
        std::cout << "未找到日志配置文件: " << configPath << "，使用默认配置" << std::endl;
    }

    initLoggerWithConfig(cfg, logger_);
    initialized_ = true;
}

void Logger::ensureInitialized()
{
    if (!initialized_) {
        std::lock_guard<std::mutex> lock(initMutex_);
        if (!initialized_) {
            LogConfig cfg;
            bool loaded = loadLogConfigFromJson(defaultConfigPath_, cfg);

            if (!loaded) {
                // 使用默认配置，只输出到控制台
                cfg.enableFileLog = false;
                cfg.level = "info";
            }

            initLoggerWithConfig(cfg, logger_);
            initialized_ = true;
        }
    }
}

std::shared_ptr<spdlog::logger> Logger::getLogger()
{
    ensureInitialized();
    return logger_;
}

void Logger::setDefaultConfigPath(const std::string &configPath)
{
    defaultConfigPath_ = configPath;
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END