#ifndef DECODER_SDK_INTERNAL_JITTER_DETECTOR_H
#define DECODER_SDK_INTERNAL_JITTER_DETECTOR_H

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>

#include "base/base_define.h"
#include "base/packet.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

/**
 * @brief 轻量级Jitter检测器，该类只能在Demuxer中使用。并且不加锁，Demuxer中使用时需注意此问题
 * 基于PTS差值与实际到达间隔的对比来检测网络抖动
 */
class JitterDetector {
public:
    struct Config {
        int64_t fallbackIntervalMs = 40;    // 当PTS不可用时的fallback间隔（毫秒），默认40ms（约25fps）
        double stabilityThresholdMs = 20.0; // 稳定性判断阈值（毫秒），默认20ms
        size_t stabilityWindowSize = 30;    // 稳定性判断窗口大小，默认30个包
        int64_t stabilityTimeoutMs = 5000;  // 稳定性检测超时时间（毫秒），默认5秒

        // 丢包策略相关参数
        bool enableDropLatePacket = true;    // 是否启用延迟包丢弃策略
        int64_t dropJitterThreshold = 200;   // 丢包抖动阈值（毫秒），默认200ms
        int64_t dropIntervalThreshold = 5;   // 丢包间隔阈值（毫秒），默认5ms
        size_t consecutiveAbnormalCount = 4; // 连续异常包数量阈值，默认4个
    };

    /**
     * @brief 丢包决策结果
     */
    struct DropDecision {
        bool shouldDrop = false; // 是否应该丢包
        std::string reason;      // 丢包原因
    };

private:
    friend class Demuxer;

    /**
     * @brief 构造函数
     * @param url 流URL，用于日志记录
     * @param config 配置项
     */
    explicit JitterDetector(const std::string &url, const Config config = Config());

    /**
     * @brief 析构函数
     */
    ~JitterDetector() = default;

    // 禁用拷贝构造和拷贝赋值
    JitterDetector(const JitterDetector &) = delete;
    JitterDetector &operator=(const JitterDetector &) = delete;

    /**
     * @brief 处理数据包，记录到达时间并计算Jitter
     * @param packet 数据包
     * @param timeBase 时间基准
     * @param dropDecision 输出丢包决策结果
     */
    void processPacket(const AVPacket *const packet, const AVRational &timeBase,
                       DropDecision &dropDecision);

    /**
     * @brief 重置检测器状态
     * @param config 配置项，可选，若提供则更新配置
     */
    void reset(const std::optional<Config> config = std::nullopt);

    /**
     * @brief 判断当前流是否稳定
     * @return true表示稳定，false表示不稳定
     */
    bool isStreamStable() const;

private:
    /**
     * @brief 计算期望间隔（基于PTS差值）
     * @param currentPts 当前包的PTS
     * @param timeBase 时间基准
     * @return 期望间隔（毫秒），如果无法计算则返回-1
     */
    int64_t calculateExpectedInterval(int64_t currentPts, AVRational timeBase) const;

    /**
     * @brief 检查流稳定性（基于固定窗口的平均jitter算法）
     *
     * @param jitter 当前抖动值（毫秒）
     */
    void checkStreamStability(int64_t jitter);

    /**
     * @brief 检查是否需要丢包
     * @param packet 数据包
     * @param jitter 当前抖动值
     * @param actualInterval 实际到达间隔
     * @return 丢包决策
     */
    DropDecision checkDropDecision(const AVPacket *const packet, int64_t jitter,
                                   int64_t actualInterval);

private:
    // 核心参数
    Config config_; // 配置参数

    // 时间记录
    std::chrono::steady_clock::time_point lastArrivalTime_;    // 上次包到达时间
    std::chrono::steady_clock::time_point stabilityStartTime_; // 开始判稳的时间
    int64_t lastPts_;                                          // 上次包的PTS
    bool hasFirstPacket_;                                      // 是否已收到第一个包

    // Jitter计算
    int64_t instantJitter_; // 最近一次jitter值（毫秒）
    int64_t maxJitter_;     // 统计窗口内最大Jitter值

    // 稳定性检测 - 使用固定窗口收集jitter样本
    std::deque<int64_t> recentJitters_; // 最近的jitter值队列，用于稳定性判断
    bool streamStable_;                 // 流是否稳定
    bool stabilityCompleted_;           // 稳定性检测是否完成

    // 丢包策略相关
    std::deque<int64_t> recentAbnormalIntervals_; // 最近的异常间隔队列

    // 统计计数
    uint64_t packetCount_;        // 处理的包数量
    uint64_t droppedPacketCount_; // 丢弃的包数量

    // 日志记录
    std::string url_; // 流URL，用于日志记录
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_JITTER_DETECTOR_H