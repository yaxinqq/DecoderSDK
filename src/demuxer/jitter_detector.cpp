#include "jitter_detector.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "logger/logger.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

JitterDetector::JitterDetector(const std::string &url, const Config config)
    : config_(config),
      lastDts_(AV_NOPTS_VALUE),
      hasFirstPacket_(false),
      averageJitter_(0.0),
      instantJitter_(0),
      maxJitter_(0),
      streamStable_(false),
      stabilityCompleted_(false),
      packetCount_(0),
      droppedPacketCount_(0),
      url_(url)
{
}

void JitterDetector::processPacket(const AVPacket *const packet, const AVRational &timeBase,
                                   DropDecision &dropDecision)
{
    if (!packet) {
        dropDecision.shouldDrop = false;
        return;
    }

    const auto currentTime = std::chrono::steady_clock::now();
    const int64_t currentDts = packet->dts;

    if (!hasFirstPacket_) {
        // 第一个包，只记录时间和DTS
        lastArrivalTime_ = currentTime;
        stabilityStartTime_ = currentTime; // 开始判稳计时
        lastDts_ = currentDts;
        hasFirstPacket_ = true;
        packetCount_ = 1;
        // 初始化Jitter值
        averageJitter_ = 0.0;
        instantJitter_ = 0;
        dropDecision.shouldDrop = false;
        return;
    }

    // 计算实际到达间隔
    const auto actualInterval =
        std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastArrivalTime_)
            .count();

    // 使用 calculateExpectedInterval 函数计算期望间隔
    int64_t expectedInterval = calculateExpectedInterval(currentDts, timeBase);

    // 如果无法从DTS计算期望间隔，使用fallback间隔
    if (expectedInterval == -1) {
        expectedInterval = config_.fallbackIntervalMs;
    }

    // 计算传输间隔差异 D(i-1,i) = (t_arrival(i) - t_arrival(i-1)) - (t_expected(i) -
    // t_expected(i-1))
    const int64_t D = actualInterval - expectedInterval;

    // 更新瞬时Jitter（最近一次Δt偏差）
    instantJitter_ = std::abs(D);

    // 使用算法更新平均Jitter估计
    // J = J + (|D(i-1,i)| - J) / 16
    averageJitter_ = averageJitter_ + (static_cast<double>(D) - averageJitter_) / 16.0;

    // 更新最大Jitter值
    maxJitter_ = std::max(maxJitter_, instantJitter_);

    // 检查流稳定性
    checkStreamStability();

    // 检查丢包决策
    dropDecision = checkDropDecision(packet, D, actualInterval);

    // 更新状态
    lastArrivalTime_ = currentTime;
    lastDts_ = currentDts;
    packetCount_++;

    if (dropDecision.shouldDrop) {
        droppedPacketCount_++;
    }
}

void JitterDetector::reset(const std::optional<Config> config)
{
    if (config.has_value()) {
        config_ = config.value();
    }

    lastDts_ = AV_NOPTS_VALUE;
    hasFirstPacket_ = false;
    averageJitter_ = 0.0;
    instantJitter_ = 0;
    maxJitter_ = 0;
    streamStable_ = false;
    stabilityCompleted_ = false;
    packetCount_ = 0;
    droppedPacketCount_ = 0;

    // 清空队列
    recentAbnormalIntervals_.clear();
}

bool JitterDetector::isStreamStable() const
{
    // 如果还没有收到第一个包，认为不稳定
    if (!hasFirstPacket_) {
        return false;
    }

    // 如果稳定性检测完成，返回检测结果
    if (stabilityCompleted_) {
        return streamStable_;
    }

    // 否则认为还在检测中，不稳定
    return false;
}

void JitterDetector::checkStreamStability()
{
    // 需要收集足够的样本才开始判断稳定性
    if (packetCount_ < config_.stabilityWindowSize) {
        return;
    }

    // 使用的averageJitter_进行稳定性判断
    if (std::abs(averageJitter_) <= config_.stabilityThresholdMs) {
        // 当前稳定
        if (!streamStable_) {
            // 从不稳定变为稳定
            streamStable_ = true;
            stabilityCompleted_ = true;

            const auto currentTime = std::chrono::steady_clock::now();
            const auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         currentTime - stabilityStartTime_)
                                         .count();

            LOG_DEBUG(
                "JitterDetector: Stream became stable. average jitter: {:.2f}ms, Packets: {}, "
                "Time elapsed: {}ms, url: {}",
                averageJitter_, packetCount_, elapsedTime, url_);
        }
    } else {
        // 当前不稳定
        if (streamStable_) {
            LOG_DEBUG(
                "JitterDetector: Stream became unstable. average jitter: {:.2f}ms > "
                "{:.2f}ms, "
                "restarting stability detection, url: {}",
                averageJitter_, config_.stabilityThresholdMs, url_);

            // 从稳定变为不稳定，重新开始检测
            streamStable_ = false;
            stabilityCompleted_ = false;
            stabilityStartTime_ = std::chrono::steady_clock::now();

            packetCount_ = 0;
            averageJitter_ = 0.0;
            hasFirstPacket_ = false;
        } else {
            // 继续不稳定
            LOG_TRACE(
                "JitterDetector: Stream unstable, average jitter: {:.2f}ms > {:.2f}ms, continuing "
                "detection, url: {}",
                averageJitter_, config_.stabilityThresholdMs, url_);
        }
    }
}

JitterDetector::DropDecision JitterDetector::checkDropDecision(const AVPacket *const packet,
                                                               int64_t jitter,
                                                               int64_t actualInterval)
{
    DropDecision decision;
    decision.shouldDrop = false;

    // 如果未启用丢包策略，直接返回
    if (!config_.enableDropLatePacket) {
        return decision;
    }

    // 检查是否为异常包（抖动过大或间隔过小）
    if (std::abs(jitter) >= config_.dropJitterThreshold ||
        actualInterval <= config_.dropIntervalThreshold) {
        recentAbnormalIntervals_.push_back(actualInterval);
    } else {
        // 正常包，清空异常记录
        recentAbnormalIntervals_.clear();
        return decision;
    }

    // 检查是否达到连续异常包阈值
    if (recentAbnormalIntervals_.size() >= config_.consecutiveAbnormalCount &&
        actualInterval <= config_.dropIntervalThreshold &&
        !(packet->flags & AV_PKT_FLAG_KEY)) { // 不是关键帧

        decision.shouldDrop = true;

        // 构造丢包原因
        char reasonBuffer[256];
        snprintf(reasonBuffer, sizeof(reasonBuffer),
                 "Network jitter detected, current jitter: %lldms", static_cast<long long>(jitter));
        decision.reason = reasonBuffer;

        // 移除最老的异常记录
        recentAbnormalIntervals_.pop_front();

        LOG_TRACE("JitterDetector: {}, url: {}", decision.reason, url_);
    }

    return decision;
}

int64_t JitterDetector::calculateExpectedInterval(int64_t currentDts, AVRational timeBase) const
{
    if (currentDts == AV_NOPTS_VALUE || lastDts_ == AV_NOPTS_VALUE || currentDts <= lastDts_) {
        return -1; // 无法计算
    }

    const int64_t dtsDiff = currentDts - lastDts_;
    const double dtsDiffSeconds = dtsDiff * av_q2d(timeBase);
    const int64_t dtsDiffMs = static_cast<int64_t>(dtsDiffSeconds * 1000.0);

    // 合理性检查
    if (dtsDiffMs >= 1 && dtsDiffMs <= 10000) {
        return dtsDiffMs;
    }

    return -1; // 不合理的间隔
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END