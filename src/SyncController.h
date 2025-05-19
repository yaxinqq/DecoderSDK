#pragma once
#include "Clock.h"
#include <chrono>
#include <memory>

class SyncController
{
public:
    enum class MasterClock
    {
        Audio,
        Video,
        External
    };

    SyncController(MasterClock type = MasterClock::Video,
                   double avSyncThreshold = 0.010, // 10 ms
                   double avSyncMaxDrift = 0.100,  // 100 ms
                   double jitterAlpha = 0.1);      // 抖动平滑系数

    void setMaster(MasterClock m) { master_ = m; }
    double getMasterClock() const;
    void setSpeed(double speed);

    void updateAudioClock(double pts, int serial = 0);
    void updateVideoClock(double pts, int serial = 0);
    void updateExternalClock(double pts, int serial = 0);
    void resetClocks();

    // 计算视频延迟（ms）
    double computeVideoDelay(double framePts,
                             double frameDuration,
                             double baseDelay,
                             double speed);

    // 计算音频延迟（ms）
    double computeAudioDelay(double audioPts,
                             double bufferDelay,
                             double speed);

private:
    MasterClock master_;
    Clock audioClock_, videoClock_, externalClock_;
    double syncThreshold_, maxDrift_, alpha_;
    double smoothedVideoDrift_ = 0.0;
    double smoothedAudioDrift_ = 0.0;
};