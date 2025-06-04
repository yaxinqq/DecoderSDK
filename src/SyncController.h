#pragma once
#include "Clock.h"
#include <atomic>
#include <chrono>
#include <memory>

class SyncController {
public:
    enum class MasterClock { Audio, Video, External };

    // 同步状态
    enum class SyncState {
        InSync,       // 同步良好
        SlightDrift,  // 轻微漂移
        OutOfSync     // 严重失步
    };

    // 同步统计信息
    struct SyncStats {
        SyncState state;
        double videoDrift;
        double audioDrift;
        double masterClock;
        int droppedFrames;
        int duplicatedFrames;
        double avgDelay;
    };

public:
    SyncController(MasterClock type = MasterClock::Video,
                   double avSyncThreshold = 0.010,  // 10 ms
                   double avSyncMaxDrift = 0.100,   // 100 ms
                   double jitterAlpha = 0.1);       // 抖动平滑系数

    // 基础设置
    void setMaster(MasterClock m);
    MasterClock master() const
    {
        return master_.load(std::memory_order_acquire);
    }

    void setAVSyncThreshold(double threshold);
    void setAdaptiveSync(bool enable)
    {
        adaptiveSync_.store(enable);
    }

    double getMasterClock() const;
    void setSpeed(double speed);

    // 时钟更新
    void updateAudioClock(double pts, int serial = 0);
    void updateVideoClock(double pts, int serial = 0);
    void updateExternalClock(double pts, int serial = 0);
    void resetClocks();

    // 改进的延迟计算
    double computeVideoDelay(double framePts, double frameDuration,
                             double baseDelay, double speed);
    double computeAudioDelay(double audioPts, double bufferDelay, double speed);

    // 新增功能
    bool shouldDropFrame(double framePts, double frameDuration) const;
    bool shouldDuplicateFrame(double framePts, double frameDuration) const;

    // 同步状态监控
    SyncState getSyncState() const;
    SyncStats getStats() const;

    // 自适应参数调整
    void updateSyncParameters();

    // 同步质量统计结构
    struct SyncQualityStats {
        int totalSyncCount;
        int goodSyncCount;
        int poorSyncCount;
        double goodSyncRate;  // 百分比
        double avgDrift;      // 平均漂移（秒）
        double maxDrift;      // 最大漂移（秒）
    };

    // 获取同步质量统计
    SyncQualityStats getSyncQualityStats() const;

private:
    // 内部方法
    double getMasterClockCached() const;
    void updateSyncQuality(double drift);
    double computeAdaptiveThreshold() const;
    SyncState evaluateSyncState(double drift) const;

private:
    std::atomic<MasterClock> master_;
    Clock audioClock_, videoClock_, externalClock_;

    // 同步参数（原子操作）
    std::atomic<double> syncThreshold_{0.0};
    std::atomic<double> maxDrift_{0.0};
    std::atomic<double> alpha_{0.0};
    std::atomic<bool> adaptiveSync_{true};

    // 同步质量统计
    std::atomic<int> goodSyncCount_{0};
    std::atomic<int> poorSyncCount_{0};
    std::atomic<double> totalDrift_{0.0};
    std::atomic<double> avgDrift_{0.0};

    // 平滑漂移值
    std::atomic<double> smoothedVideoDrift_{0.0};
    std::atomic<double> smoothedAudioDrift_{0.0};

    // 统计信息
    std::atomic<int> droppedFrames_{0};
    std::atomic<int> duplicatedFrames_{0};
    std::atomic<double> avgVideoDelay_{0.0};
    std::atomic<double> avgAudioDelay_{0.0};

    // 性能优化
    mutable std::atomic<double> cachedMasterClock_{0.0};
    mutable std::atomic<int64_t> masterClockCacheTime_{0};

    // 自适应参数
    std::atomic<int> syncQualityCounter_{0};
    std::atomic<double> adaptiveThreshold_{0.010};

    // 常量
    static constexpr int64_t kMasterClockCacheValidityUs = 5000;  // 5ms
    static constexpr int kSyncQualityWindow = 100;
    static constexpr double kMinSyncThreshold = 0.005;  // 5ms
    static constexpr double kMaxSyncThreshold = 0.050;  // 50ms
};