#ifndef DECODER_SDK_INTERNAL_LOGGER_H
#define DECODER_SDK_INTERNAL_LOGGER_H
#include <memory>
#include <mutex>
#include <string>

#include <spdlog/spdlog.h>

#include "base/base_define.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class Logger {
public:
    /**
     * @brief 初始化日志系统（可选调用，如果不调用会使用默认配置）
     *
     * @param configFile 配置文件路径
     */
    static void initFromConfig(const std::string &configFile);

    /**
     * @brief 获取日志对象（自动初始化）
     *
     * @return std::shared_ptr<spdlog::logger> 日志对象
     */
    static std::shared_ptr<spdlog::logger> getLogger();

    /**
     * @brief 设置默认配置文件路径
     *
     * @param configPath 配置文件路径
     */
    static void setDefaultConfigPath(const std::string &configPath);

private:
    /**
     * @brief 内部初始化方法
     */
    static void ensureInitialized();

private:
    // 日志指针
    static std::shared_ptr<spdlog::logger> logger_;
    // 初始化标志
    static bool initialized_;
    // 线程安全锁
    static std::mutex initMutex_;
    // 默认配置文件路径
    static std::string defaultConfigPath_;
};

#define LOG_TRACE(...) Logger::getLogger()->trace(__VA_ARGS__)
#define LOG_DEBUG(...) Logger::getLogger()->debug(__VA_ARGS__)
#define LOG_INFO(...) Logger::getLogger()->info(__VA_ARGS__)
#define LOG_WARN(...) Logger::getLogger()->warn(__VA_ARGS__)
#define LOG_ERROR(...) Logger::getLogger()->error(__VA_ARGS__)
#define LOG_FATAL(...) Logger::getLogger()->critical(__VA_ARGS__)

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_LOGGER_H