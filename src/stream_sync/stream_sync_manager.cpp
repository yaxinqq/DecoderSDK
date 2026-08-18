#include "stream_sync_manager.h"

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavutil/time.h>
}

#include "logger/logger.h"
#include "utils/common_utils.h"

namespace {
// 常量
constexpr int64_t kMasterClockCacheValidityUs = 5000; // 主时钟缓存有效期（微秒）
constexpr int kSyncQualityWindow = 100;               // 同步质量窗口大小
constexpr double kMinSyncThreshold = 0.005;           // 最小同步阈值（秒）
constexpr double kMaxSyncThreshold = 0.050;           // 最大同步阈值（秒）

double atomicFetchAdd(std::atomic<double> &atomic, double delta)
{
    double old = atomic.load();
    double desired;
    while (true) {
        desired = old + delta;
        if (atomic.compare_exchange_weak(old, desired)) {
            return old;
        }
        // old 会被更新为当前值
    }
}

// EMA 平滑函数（改进版）
double smoothEMA(double alpha, double prev, double current, double speed, double maxChange = 0.1)
{
    // 根据速度调整参数
    double speedFactor = std::min(speed, 4.0); // 限制最大影响
    double adjustedMaxChange = maxChange * speedFactor;
    double adjustedAlpha = std::min(alpha * speedFactor, 0.9); // 限制最大alpha

    // 检测方向变化（从正变负或从负变正）
    bool directionChange = (prev > 0 && current < 0) || (prev < 0 && current > 0);

    // 方向变化时使用更大的变化量限制
    double effectiveMaxChange = directionChange ? adjustedMaxChange * 2.0 : adjustedMaxChange;

    // 限制单次变化量
    double change = std::clamp(current - prev, -effectiveMaxChange, effectiveMaxChange);

    // 方向变化时使用更大的alpha值加速收敛
    double effectiveAlpha = directionChange ? std::min(adjustedAlpha * 2.0, 1.0) : adjustedAlpha;

    // 计算新值
    double newValue = prev + effectiveAlpha * change;

    // 限制累积漂移范围，根据速度调整
    return std::clamp(newValue, -0.2 * speedFactor, 0.2 * speedFactor);
}
} // namespace

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

StreamSyncManager::StreamSyncManager(ClockType master, double syncThreshold, double maxDrift,
                                     double jitterAlpha)
    : master_(master), syncThreshold_(syncThreshold), maxDrift_(maxDrift), alpha_(jitterAlpha)
{
    videoClock_.init(0);
    audioClock_.init(0);
    externalClock_.reset();
    adaptiveThreshold_.store(syncThreshold);
}

void StreamSyncManager::setMaster(ClockType m)
{
    master_.store(m, std::memory_order_release);
}

ClockType StreamSyncManager::master() const
{
    return master_.load(std::memory_order_acquire);
}

void StreamSyncManager::setAVSyncThreshold(double threshold)
{
    syncThreshold_.store(std::clamp(threshold, kMinSyncThreshold, kMaxSyncThreshold),
                         std::memory_order_release);
}

void StreamSyncManager::setAdaptiveSync(bool enable)
{
    adaptiveSync_.store(enable);
}

void StreamSyncManager::setSpeed(double speed)
{
    audioClock_.setClockSpeed(speed);
    videoClock_.setClockSpeed(speed);
    externalClock_.setSpeed(speed);
}

void StreamSyncManager::updateAudioClock(double pts, uint64_t serial)
{
    audioClock_.setClock(pts, serial);
    audioClock_.calibrate();
}

void StreamSyncManager::updateVideoClock(double pts, uint64_t serial)
{
    videoClock_.setClock(pts, serial);
    videoClock_.calibrate();
}

void StreamSyncManager::resetClocks()
{
    audioClock_.reset();
    videoClock_.reset();
    externalClock_.reset();

    // 重置统计信息
    smoothedVideoDrift_.store(0.0, std::memory_order_release);
    smoothedAudioDrift_.store(0.0, std::memory_order_release);
    droppedFrames_.store(0, std::memory_order_release);
    duplicatedFrames_.store(0, std::memory_order_release);
    syncQualityCounter_.store(0, std::memory_order_release);
}

void StreamSyncManager::externalClockSeekTo(double pts)
{
    externalClock_.seekTo(pts);
}

void StreamSyncManager::externalClockSetPaused(bool paused)
{
    externalClock_.setPaused(paused);
}

double StreamSyncManager::computeVideoDelay(double framePts, double frameDuration, double baseDelay,
                                            double speed)
{
    // 如果当前主时钟是视频时钟，直接返回默认延迟
    if (master_.load() == ClockType::kVideo) {
        return baseDelay;
    }

    // 主时钟
    double master = getMasterClock();
    double diff = framePts - master; // PTS 与主时钟的差值 (秒)

    // 默认延迟
    double delay = 0.0;

    // 偏差阈值，避免过度抖动
    double threshold = syncThreshold_.load() / speed;

    if (std::abs(diff) > threshold) {
        if (diff > 0) {
            // 视频比音频超前 -> 延长等待
            delay += diff * 1000.0 / speed;
        } else {
            // 视频比音频落后 -> 减少等待
            delay = std::max(0.0, delay + diff * 1000.0);
        }
    }

   /* LOG_INFO("frame pts: {}, master: {}, diff: {}, baseDelay: {}, finalDelay: {}", framePts,
       master, diff, baseDelay, delay);*/

    return delay; // 单位: 毫秒
}

bool StreamSyncManager::shouldDropFrame(double framePts, double frameDuration) const
{
    double drift = smoothedVideoDrift_.load(std::memory_order_acquire);
    double threshold = syncThreshold_.load();
    double speed = videoClock_.speed(); // 获取当前速度

    // 考虑速度因素调整阈值
    threshold /= speed;

    // 更保守的丢帧策略
    return (drift < -threshold * 3.0) && (frameDuration < 0.033 / speed);
}

bool StreamSyncManager::shouldDuplicateFrame(double framePts, double frameDuration) const
{
    double drift = smoothedVideoDrift_.load(std::memory_order_acquire);
    double threshold = syncThreshold_.load();
    double speed = videoClock_.speed(); // 获取当前速度

    // 考虑速度因素调整阈值
    threshold /= speed;

    // 只有在严重超前且帧持续时间较长时才重复帧
    return (drift > threshold * 3.0) && (frameDuration > 0.020 / speed); // 调整帧率阈值
}

SyncState StreamSyncManager::getSyncState() const
{
    double videoDrift = std::abs(smoothedVideoDrift_.load(std::memory_order_acquire));
    double audioDrift = std::abs(smoothedAudioDrift_.load(std::memory_order_acquire));
    double maxDrift = std::max(videoDrift, audioDrift);

    return evaluateSyncState(maxDrift);
}

SyncStats StreamSyncManager::getStats() const
{
    return {getSyncState(),
            smoothedVideoDrift_.load(std::memory_order_acquire),
            smoothedAudioDrift_.load(std::memory_order_acquire),
            getMasterClock(),
            droppedFrames_.load(std::memory_order_acquire),
            duplicatedFrames_.load(std::memory_order_acquire),
            (avgVideoDelay_.load() + avgAudioDelay_.load()) / 2.0};
}

void StreamSyncManager::updateSyncParameters()
{
    if (!adaptiveSync_.load()) {
        return;
    }

    // 根据同步质量调整参数
    int quality = syncQualityCounter_.load(std::memory_order_acquire);
    if (quality > kSyncQualityWindow) {
        double newThreshold = computeAdaptiveThreshold();
        adaptiveThreshold_.store(newThreshold, std::memory_order_release);
        syncQualityCounter_.store(0, std::memory_order_release);
    }
}

SyncQualityStats StreamSyncManager::getSyncQualityStats() const
{
    int total = syncQualityCounter_.load(std::memory_order_acquire);
    int good = goodSyncCount_.load(std::memory_order_acquire);
    int poor = poorSyncCount_.load(std::memory_order_acquire);

    return {total,
            good,
            poor,
            total > 0 ? (static_cast<double>(good) / total * 100.0) : 0.0,
            avgDrift_.load(std::memory_order_acquire),
            maxDrift_.load(std::memory_order_acquire)};
}

double StreamSyncManager::getClock(ClockType clock) const
{
    double masterClock;

    switch (clock) {
        case ClockType::kAudio:
            masterClock = audioClock_.getClock();
            break;
        case ClockType::kVideo:
            masterClock = videoClock_.getClock();
            break;
        case ClockType::kExternal:
            masterClock = externalClock_.getClock();
            break;
        default:
            masterClock = audioClock_.getClock();
            break;
    }

    return masterClock;
}

double StreamSyncManager::getMasterClock() const
{
    return getClock(master_.load());
}

void StreamSyncManager::updateSyncQuality(double drift)
{
    // 递增总的同步质量计数器
    int currentCount = syncQualityCounter_.fetch_add(1, std::memory_order_acq_rel) + 1;

    // 根据漂移值评估同步质量
    double threshold = syncThreshold_.load(std::memory_order_acquire);

    // 更新漂移统计
    atomicFetchAdd(totalDrift_, drift);

    // 更新最大漂移值
    double currentMax = maxDrift_.load(std::memory_order_acquire);
    while (drift > currentMax &&
           !maxDrift_.compare_exchange_weak(currentMax, drift, std::memory_order_release,
                                            std::memory_order_acquire)) {
        // 循环直到成功更新
    }

    // 更新平均漂移值
    double newAvg = totalDrift_.load(std::memory_order_acquire) / currentCount;
    avgDrift_.store(newAvg, std::memory_order_release);

    // 根据漂移值分类统计
    if (drift <= threshold) {
        goodSyncCount_.fetch_add(1, std::memory_order_acq_rel);
    } else {
        poorSyncCount_.fetch_add(1, std::memory_order_acq_rel);

        // 严重失步时记录警告
        if (drift > threshold * 3.0) {
            LOG_WARN("Severe sync drift detected: {:.3f}ms (threshold: {:.3f}ms)", drift * 1000,
                     threshold * 1000);
        }
    }

    // 周期性质量报告
    if (currentCount % 500 == 0) {
        double goodRate = static_cast<double>(goodSyncCount_.load()) / currentCount * 100.0;
        LOG_INFO(
            "Sync Quality Report - Total: {}, Good: {:.1f}%, Avg Drift: "
            "{:.3f}ms, Max Drift: {:.3f}ms",
            currentCount, goodRate, newAvg * 1000, currentMax * 1000);
    }
}

double StreamSyncManager::computeAdaptiveThreshold() const
{
    double videoDrift = std::abs(smoothedVideoDrift_.load(std::memory_order_acquire));
    double audioDrift = std::abs(smoothedAudioDrift_.load(std::memory_order_acquire));
    double avgDrift = (videoDrift + audioDrift) / 2.0;

    // 根据平均漂移调整阈值
    double baseThreshold = syncThreshold_.load(std::memory_order_acquire);
    double adaptiveThreshold = baseThreshold + avgDrift * 0.5;

    return std::clamp(adaptiveThreshold, kMinSyncThreshold, kMaxSyncThreshold);
}

SyncState StreamSyncManager::evaluateSyncState(double drift) const
{
    double threshold = syncThreshold_.load(std::memory_order_acquire);

    if (drift < threshold) {
        return SyncState::kInSync;
    } else if (drift < threshold * 3.0) {
        return SyncState::kSlightDrift;
    } else {
        return SyncState::kOutOfSync;
    }
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END