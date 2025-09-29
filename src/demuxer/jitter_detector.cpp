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
    const int64_t currentDts = packet->pts;

    if (!hasFirstPacket_) {
        // 第一个包，只记录时间和DTS
        lastArrivalTime_ = currentTime;
        stabilityStartTime_ = currentTime; // 开始判稳计时
        lastDts_ = currentDts;
        hasFirstPacket_ = true;
        packetCount_ = 1;
        // 初始化Jitter值
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

    // 计算jitter = 实际间隔 - 期望间隔
    const int64_t jitter = actualInterval - expectedInterval;

    // 更新瞬时Jitter（绝对值）
    instantJitter_ = std::abs(jitter);

    // 更新最大Jitter值
    maxJitter_ = std::max(maxJitter_, instantJitter_);

    // 检查流稳定性
    checkStreamStability(instantJitter_);

    // 检查丢包决策
    dropDecision = checkDropDecision(packet, jitter, actualInterval);

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
    instantJitter_ = 0;
    maxJitter_ = 0;
    streamStable_ = false;
    stabilityCompleted_ = false;
    packetCount_ = 0;
    droppedPacketCount_ = 0;

    // 清空队列
    recentJitters_.clear();
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

void JitterDetector::checkStreamStability(int64_t jitter)
{
    // 如果稳定性检测已完成，不再重复检测
    if (stabilityCompleted_) {
        return;
    }

    // 收集jitter样本到固定大小的窗口中
    if (recentJitters_.size() < config_.stabilityWindowSize) {
        recentJitters_.push_back(jitter);

        // 如果样本数量还不够，继续收集
        if (recentJitters_.size() < config_.stabilityWindowSize) {
            return;
        }
    }

    // 当窗口满了，计算平均jitter
    double avgJitter =
        std::accumulate(recentJitters_.begin(), recentJitters_.end(), 0.0) / recentJitters_.size();

    if (avgJitter > config_.stabilityThresholdMs) {
        // 平均jitter超过阈值，移除最老的样本，继续收集
        recentJitters_.pop_front();
        LOG_INFO(
            "JitterDetector: Stream unstable, average jitter: {:.2f}ms > {:.2f}ms, continuing "
            "detection, url: {}",
            avgJitter, config_.stabilityThresholdMs, url_);
    } else {
        // 平均jitter在阈值内，流稳定
        streamStable_ = true;
        stabilityCompleted_ = true;

        const auto currentTime = std::chrono::steady_clock::now();
        const auto elapsedTime =
            std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - stabilityStartTime_)
                .count();

        LOG_INFO(
            "JitterDetector: Stream became stable. average jitter: {:.2f}ms, Packets: {}, "
            "Time elapsed: {}ms, url: {}",
            avgJitter, packetCount_, elapsedTime, url_);
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

    // 检查是否达到连续异常包阈值且当前间隔过小且不是关键帧
    if (recentAbnormalIntervals_.size() >= config_.consecutiveAbnormalCount &&
        !(packet->flags & AV_PKT_FLAG_KEY)) {
        // 计算到达时间平均值
        const auto averageActualInterval =
            std::accumulate(recentAbnormalIntervals_.begin(), recentAbnormalIntervals_.end(), 0.0) /
            recentAbnormalIntervals_.size();

        if (averageActualInterval <= config_.dropIntervalThreshold) {
            decision.shouldDrop = true;

            // 构造丢包原因，记录最近几个异常间隔
            char reasonBuffer[512];
            std::string intervals;
            for (size_t i = 0; i < std::min(recentAbnormalIntervals_.size(), size_t(4)); ++i) {
                if (i > 0)
                    intervals += ", ";
                intervals += std::to_string(recentAbnormalIntervals_[i]);
            }

            snprintf(
                reasonBuffer, sizeof(reasonBuffer),
                "Network jitter detected, recent intervals: [%s], current jitter: %lldms, actual "
                "interval: %lldms",
                intervals.c_str(), static_cast<long long>(jitter),
                static_cast<long long>(actualInterval));
            decision.reason = reasonBuffer;

            // 清除当前的异常记录
            recentAbnormalIntervals_.pop_front();

            LOG_INFO("JitterDetector: {}, url: {}", decision.reason, url_);
        }
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