#ifndef DECODER_SDK_INTERNAL_LOGGER_SINK_H
#define DECODER_SDK_INTERNAL_LOGGER_SINK_H

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <spdlog/details/file_helper.h>
#include <spdlog/details/os.h>
#include <spdlog/sinks/base_sink.h>

#include "base/base_define.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

/**
 * @brief 自定义混合轮转文件sink
 *
 * 该类实现了基于时间和文件大小的混合轮转策略：
 * 1. 时间轮转：支持按小时或按天轮转
 * 2. 大小轮转：当文件大小超过限制时进行轮转
 * 3. 文件夹清理：支持按文件夹总大小和保留天数进行清理
 * 4. 级别过滤：支持只记录特定级别的日志或所有级别的日志
 */
template <typename Mutex>
class LoggerSink : public spdlog::sinks::base_sink<Mutex> {
public:
    /**
     * @brief 构造函数
     * @param logDir 日志存放的路径
     * @param maxSize 单个文件最大大小（字节）
     * @param maxFolderSizeMb 文件夹最大大小（MB）
     * @param hourlyRotation 是否按小时轮转（true=按小时，false=按天）
     * @param retentionDays 文件保留天数
     * @param targetLevel 目标日志级别（仅当isAllSink=false时生效）
     * @param isAllSink 是否为聚合sink（true=接收所有级别，false=仅接收targetLevel级别）
     */
    LoggerSink(const std::string &logDir, size_t maxSize, size_t maxFolderSizeMb,
               bool hourlyRotation = false, int retentionDays = 30,
               spdlog::level::level_enum targetLevel = spdlog::level::trace, bool isAllSink = false)
        : logDir_(logDir),
          maxSize_(maxSize),
          overallFilesSizeMb_(maxFolderSizeMb),
          retentionDays_(retentionDays),
          hourlyRotation_(hourlyRotation),
          targetLevel_(targetLevel),
          isAllSink_(isAllSink),
          currentSize_(0),
          fileCounter_(0)
    {
        // 确保目录存在
        spdlog::details::os::create_dir(logDir_);

        // 打开初始文件
        openCurrentFile();

        // 计算下次时间轮转的时间点
        if (hourlyRotation_) {
            nextRotationTime_ = getNextHourRotationTime();
        } else {
            nextRotationTime_ = getNextDayRotationTime();
        }
    }

protected:
    /**
     * @brief 日志记录核心函数
     * @param msg 日志消息对象
     *
     * 处理流程：
     * 1. 进行级别过滤
     * 2. 检查是否需要轮转（时间或大小）
     * 3. 格式化并写入日志
     * 4. 更新当前文件大小
     */
    void sink_it_(const spdlog::details::log_msg &msg) override
    {
        // 级别过滤：all sink接收所有级别的日志，其他sink只接收指定级别的日志
        if (!isAllSink_ && msg.level != targetLevel_) {
            return;
        }

        bool shouldRotate = false;

        // 检查时间轮转条件
        if (msg.time >= nextRotationTime_) {
            shouldRotate = true;
        }

        // 检查大小轮转条件
        if (currentSize_ >= maxSize_) {
            shouldRotate = true;
        }

        // 执行文件轮转
        if (shouldRotate) {
            rotateFile();
            // 重新计算下次轮转时间
            if (hourlyRotation_) {
                nextRotationTime_ = getNextHourRotationTime();
            } else {
                nextRotationTime_ = getNextDayRotationTime();
            }
        }

        // 格式化日志消息并写入文件
        spdlog::memory_buf_t formatted;
        this->formatter_->format(msg, formatted);
        fileHelper_.write(formatted);

        // 更新当前文件大小（formatted.size()返回字节数）
        currentSize_ += formatted.size();
    }

    /**
     * @brief 刷新缓冲区
     * 强制将缓冲区中的数据写入磁盘
     */
    void flush_() override
    {
        fileHelper_.flush();
    }

private:
    /**
     * @brief 打开当前日志文件
     *
     * 文件命名规则：
     * - 时间轮转：basename_YYYYMMDDHHMMSS.log
     * - 大小轮转：在时间轮转文件名基础上添加序号，如 .1, .2, .3...
     */
    void openCurrentFile()
    {
        std::string filename;
        if (fileCounter_ == 0) {
            const auto now = std::chrono::system_clock::now();
            const auto time_t = std::chrono::system_clock::to_time_t(now);

            std::tm tm;
#ifdef _WIN32
            localtime_s(&tm, &time_t);
#else
            localtime_r(&time_t, &tm); // POSIX/Linux
#endif

            // 时间轮转的文件名格式：basename_YYYYMMDDHHMMSS.log
            filename = fmt::format("{}_{:04d}{:02d}{:02d}{:02d}{:02d}{:02d}",
                                   transTargetLevelToString(), tm.tm_year + 1900, tm.tm_mon + 1,
                                   tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
            currentFileName = filename;
        } else {
            // 大小轮转的文件名格式：在当前文件名基础上添加序号
            filename = fmt::format("{}.{}", currentFileName, fileCounter_);
        }

        // 打开文件并获取当前大小
        fileHelper_.open(logDir_ + "/" + filename + ".log");
        currentSize_ = fileHelper_.size();
    }

    /**
     * @brief 执行文件轮转
     *
     * 轮转流程：
     * 1. 关闭当前文件
     * 2. 根据轮转原因更新文件计数器
     * 3. 打开新文件
     * 4. 清理旧文件
     */
    void rotateFile()
    {
        // 关闭当前文件
        fileHelper_.close();

        // 根据轮转原因更新计数器
        if (currentSize_ >= maxSize_) {
            // 大小轮转：增加计数器，在同一时间段内创建多个文件
            fileCounter_++;
        } else {
            // 时间轮转：重置计数器，开始新的时间段
            fileCounter_ = 0;
        }

        // 打开新文件
        openCurrentFile();

        // 执行文件清理
        cleanupOldFiles();
    }

    /**
     * @brief 清理旧的日志文件
     *
     * 清理策略：
     * 1. 按文件夹总大小清理：删除最旧的文件直到总大小满足限制
     * 2. 按保留天数清理：删除超过保留期限的文件
     *
     * 注意：只清理与当前sink级别相关的日志文件（基于级别名称匹配）
     */
    void cleanupOldFiles()
    {
        try {
            if (!std::filesystem::exists(logDir_)) {
                return;
            }

            // 收集当前级别相关的日志文件信息
            std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> files;
            size_t totalSize = 0;

            // 根据当前sink的级别确定文件名前缀
            const std::string levelPrefix = transTargetLevelToString();

            // 遍历目录，查找当前级别的所有日志文件
            for (const auto &entry : std::filesystem::directory_iterator(logDir_)) {
                if (entry.is_regular_file()) {
                    auto filename = entry.path().filename().string();

                    // 检查文件名是否以当前级别前缀开头
                    // 文件名格式：level_YYYYMMDDHHMMSS.log 或 level_YYYYMMDDHHMMSS.log.N
                    if (filename.find(levelPrefix + "_") == 0) {
                        files.emplace_back(entry.path(), entry.last_write_time());
                        totalSize += entry.file_size();
                    }
                }
            }

            // 按修改时间排序，最新的文件在前面
            std::sort(files.begin(), files.end(),
                      [](const auto &a, const auto &b) { return a.second > b.second; });

            // 1. 按文件夹大小限制进行清理（仅针对当前级别的文件）
            const size_t overllFilesSize = overallFilesSizeMb_ * 1024 * 1024; // 转换为字节

            if (totalSize > overllFilesSize) {
                // 从最旧的文件开始删除，直到满足大小限制
                for (auto it = files.rbegin(); it != files.rend() && totalSize > overllFilesSize;
                     ++it) {
                    try {
                        const size_t file_size = std::filesystem::file_size(it->first);
                        std::filesystem::remove(it->first);
                        totalSize -= file_size;
                    } catch (const std::exception &) {
                        // 忽略删除失败的文件，继续处理其他文件
                    }
                }
            }

            // 2. 按保留天数进行清理
            const auto now = std::chrono::system_clock::now();
            const auto retention_duration = std::chrono::hours(24 * retentionDays_);

            for (const auto &fileInfo : files) {
                const auto fileTime =
                    std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        fileInfo.second - std::filesystem::file_time_type::clock::now() +
                        std::chrono::system_clock::now());
                if (now - fileTime > retention_duration) {
                    try {
                        std::filesystem::remove(fileInfo.first);
                    } catch (const std::exception &) {
                        // 忽略删除失败的文件
                    }
                }
            }
        } catch (const std::exception &) {
            // 清理失败不应影响日志写入，静默处理异常
        }
    }

    /**
     * @brief 计算下一个小时轮转的时间点
     * @return 下一个整点时间
     */
    std::chrono::system_clock::time_point getNextHourRotationTime()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t now_t = std::chrono::system_clock::to_time_t(now);

        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now_t);
#else
        localtime_r(&now_c, &tm);
#endif

        tm.tm_min = 0;
        tm.tm_sec = 0;
        tm.tm_hour += 1;

        const std::time_t nextHourTime = std::mktime(&tm);
        return std::chrono::system_clock::from_time_t(nextHourTime);
    }

    /**
     * @brief 计算下一个天轮转的时间点
     * @return 下一天的00:00:00时间
     */
    std::chrono::system_clock::time_point getNextDayRotationTime()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t now_t = std::chrono::system_clock::to_time_t(now);

        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now_t);
#else
        localtime_r(&now_c, &tm);
#endif

        tm.tm_hour = 0;
        tm.tm_min = 0;
        tm.tm_sec = 0;
        tm.tm_mday += 1;

        const std::time_t nextDayTime = std::mktime(&tm);
        return std::chrono::system_clock::from_time_t(nextDayTime);
    }

    std::string transTargetLevelToString() const
    {
        if (isAllSink_) {
            return "all";
        }

        switch (targetLevel_) {
            case spdlog::level::trace:
                return "trace";
            case spdlog::level::debug:
                return "debug";
            case spdlog::level::info:
                return "info";
            case spdlog::level::warn:
                return "warning";
            case spdlog::level::err:
                return "error";
            case spdlog::level::critical:
                return "critical";
            default:
                break;
        }
        return "unknown";
    }

private:
    // 文件相关
    std::string logDir_;         // 日志文件夹地址
    std::string currentFileName; // 当前的文件名，不加计数后缀和文件后缀

    // 轮转配置
    size_t maxSize_;            // 单个文件最大大小（字节）
    size_t overallFilesSizeMb_; // 文件夹大小限制（MB）
    int retentionDays_;         // 文件保留天数
    bool hourlyRotation_;       // 是否按小时轮转（true=小时，false=天）

    // 过滤配置
    spdlog::level::level_enum targetLevel_; // 目标日志级别
    bool isAllSink_;                        // 是否为聚合sink

    // 运行时状态
    size_t currentSize_;                                     // 当前文件大小（字节）
    size_t fileCounter_;                                     // 文件序号计数器
    std::chrono::system_clock::time_point nextRotationTime_; // 下次时间轮转的时间点

    // spdlog组件
    spdlog::details::file_helper fileHelper_; // spdlog文件操作助手
};

// 使用std::mutex的线程安全版本
using LoggerSinkMutex = LoggerSink<std::mutex>;

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_LOGGER_SINK_H