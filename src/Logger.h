#pragma once
#include <spdlog/spdlog.h>
#include <memory>
#include <string>

class Logger {
   public:
    static void initFromConfig(const std::string& configFile);

    static std::shared_ptr<spdlog::logger> getLogger();

   private:
    static std::shared_ptr<spdlog::logger> logger;
};

#define LOG_TRACE(...) Logger::getLogger()->trace(__VA_ARGS__)
#define LOG_DEBUG(...) Logger::getLogger()->debug(__VA_ARGS__)
#define LOG_INFO(...) Logger::getLogger()->info(__VA_ARGS__)
#define LOG_WARN(...) Logger::getLogger()->warn(__VA_ARGS__)
#define LOG_ERROR(...) Logger::getLogger()->error(__VA_ARGS__)
#define LOG_FATAL(...) Logger::getLogger()->critical(__VA_ARGS__)