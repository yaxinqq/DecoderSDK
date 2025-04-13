#ifndef DECODER_SDK_LOGGER_H
#define DECODER_SDK_LOGGER_H

#include <spdlog/spdlog.h>
#include <memory>
#include <string>

#include "base/define.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class Logger {
public:
    static void initFromConfig(const std::string& configFile);

    static std::shared_ptr<spdlog::logger> getLogger();

private:
    static std::shared_ptr<spdlog::logger> logger;
};

#define LOG_TRACE(...) Logger::getLogger()->trace(__VA_ARGS__)
#define LOG_DEBUG(...) Logger::getLogger()->debug(__VA_ARGS__)
#define LOG_INFO(...)  Logger::getLogger()->info(__VA_ARGS__)
#define LOG_WARN(...)  Logger::getLogger()->warn(__VA_ARGS__)
#define LOG_ERROR(...) Logger::getLogger()->error(__VA_ARGS__)
#define LOG_FATAL(...) Logger::getLogger()->critical(__VA_ARGS__)

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END



#endif // DECODER_SDK_LOGGER_H