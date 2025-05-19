#include "SyncController.h"
#include "Utils.h"
#include <algorithm>
#include <cmath>

extern "C"
{
#include <libavutil/time.h>
}

namespace
{
    constexpr double kAVSyncThreshold = 0.01;
    constexpr double kAVNoSyncThreshold = 10.0;
    constexpr double kMaxFrameDuration = 10.0;
}

SyncController::SyncController(MasterClock master, double syncThreshold, double maxDrift, double jitterAlpha)
    : master_(master)
    , syncThreshold_(syncThreshold)
    , maxDrift_(maxDrift)
    , alpha_(jitterAlpha)
{
    videoClock_.init(0);
    audioClock_.init(0);
    externalClock_.init(0);
}

void SyncController::setSpeed(double speed) {
    audioClock_.setClockSpeed(speed);
    videoClock_.setClockSpeed(speed);
    externalClock_.setClockSpeed(speed);
}

void SyncController::updateAudioClock(double pts, int serial)
{
    audioClock_.setClock(pts, serial);
}

void SyncController::updateVideoClock(double pts, int serial)
{
    videoClock_.setClock(pts, serial);
}

void SyncController::updateExternalClock(double pts, int serial)
{
    externalClock_.setClock(pts, serial);
}

void SyncController::resetClocks()
{
    audioClock_.reset();
    videoClock_.reset();
    externalClock_.reset();
}

double SyncController::getMasterClock() const {
    switch (master_) {
        case MasterClock::Audio:    
            return audioClock_.getClock();
        case MasterClock::Video:    
            return videoClock_.getClock();
        case MasterClock::External: 
            return externalClock_.getClock();
    }
    return audioClock_.getClock();
}

// EMA 平滑函数
double smooth(double alpha, double prev, double current) {
    return alpha * current + (1.0 - alpha) * prev;
}

#include <iostream>
double SyncController::computeVideoDelay(double framePts,
                                         double frameDuration,
                                         double baseDelay,
                                         double speed)
{
    // 1. 更新视频时钟并按 speed 推进
    updateVideoClock(framePts, videoClock_.serial());
    videoClock_.setClockSpeed(speed);
    double master = getMasterClock();

    // 2. 计算帧相对于主时钟的偏差（秒）
    double diff = framePts - master;

    // 3. EMA 抖动平滑
    smoothedVideoDrift_ = smooth(alpha_, smoothedVideoDrift_, diff);

    // 4. 计算阈值（秒）和裁剪漂移
    double thresh = syncThreshold_ / speed;
    double drift = std::clamp(smoothedVideoDrift_, -maxDrift_, maxDrift_);

    // 5. 丢帧判断：如果帧太晚，落后超过阈值，就返回一个负值，表示需要丢弃
    if (!utils::greaterAndEqual(drift, -thresh)) {
        std::cout << "drop frame!!!!!!!" << std::endl;
        return -1.0;  
    }

    // 6. 否则按需调整延迟
    double delay = baseDelay;
    if (std::fabs(drift) > thresh) {
        delay += drift * 1000.0;  // 转为毫秒
    }

    std::cout << "framePts: " << framePts << ", master: " << master << ", diff: " << diff << ", drift: " << drift << ", baseDelay: " << baseDelay << ", delay: " << delay << std::endl;

    return !utils::greaterAndEqual(delay, 0.0) ? 0.0 : delay;
}

double SyncController::computeAudioDelay(double audioPts,
                                         double bufferDelay,
                                         double speed)
{
    // 1) 更新音频时钟，并推进到当前 pts
    const_cast<Clock&>(audioClock_).setClock(audioPts, audioClock_.queueSerial());
    const_cast<Clock&>(audioClock_).setClockSpeed(speed);

    // 2) 拿到主时钟（秒）
    double master = getMasterClock();

    // 3) 计算漂移（秒）
    double diff = audioPts - master;

    // 4) 抖动阈值（秒）
    double thresh = syncThreshold_ / speed;

    // 默认延迟就是“还有多少 ms 在缓冲”
    double delay = bufferDelay;

    // if (diff > thresh) {
    //     // 4a) 音频快了，补偿 diff
    //     delay += diff * 1000.0;
    // } else if (bufferDelay > thresh * 1000.0) {
    //     // 4b) 缓冲过多，缩短延迟（负值会让 sleep 更短）
    //     double extra = std::min(bufferDelay, maxDrift_ * 1000.0);
    //     delay -= extra;
    // }

    std::cout << "audioPts: " << audioPts << ", bufferDelay: " << bufferDelay << ", master: " << master << ", diff: " << diff << ", delay: " << delay  << ", speed: " << speed << std::endl;

    // 5) 不允许负延迟
    return delay > 0.0 ? delay : 0.0;
}
