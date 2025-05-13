#include "SyncController.h"
#include <cmath>

extern "C" {
#include <libavutil/time.h>
}

namespace {
    constexpr double kAVSyncThreshold = 0.01;   // 10ms同步阈值
    constexpr double kAVNoSyncThreshold = 10.0; // 10秒无同步阈值
    constexpr double kMaxFrameDuration = 10.0;  // 最大帧持续时间，防止异常值
}

SyncController::SyncController()
    : masterClockType_(MasterClock::Audio)
    , audioClock_(nullptr)
    , videoClock_(nullptr)
    , externalClock_(nullptr)
    , lastFramePts_(0.0)
    , frameTimer_(0.0)
{
}

SyncController::~SyncController() = default;

void SyncController::setMasterClockType(MasterClock type)
{
    masterClockType_ = type;
}

SyncController::MasterClock SyncController::getMasterClockType() const
{
    return masterClockType_;
}

Clock* SyncController::getMasterClock()
{
    switch (masterClockType_) {
        case MasterClock::Audio:
            return audioClock_;
        case MasterClock::Video:
            return videoClock_;
        case MasterClock::External:
            return externalClock_;
        default:
            return audioClock_; // 默认使用音频时钟
    }
}

void SyncController::setAudioClock(Clock* clock)
{
    audioClock_ = clock;
}

void SyncController::setVideoClock(Clock* clock)
{
    videoClock_ = clock;
}

void SyncController::setExternalClock(Clock* clock)
{
    externalClock_ = clock;
}

void SyncController::syncVideoToMaster()
{
    if (videoClock_ && getMasterClock() && videoClock_ != getMasterClock()) {
        videoClock_->syncClockToSlave(*getMasterClock());
    }
}

void SyncController::syncAudioToMaster()
{
    if (audioClock_ && getMasterClock() && audioClock_ != getMasterClock()) {
        audioClock_->syncClockToSlave(*getMasterClock());
    }
}

double SyncController::computeVideoDelay(double pts, double duration)
{
    double delay = 0.0;
    
    // 初始化帧计时器
    if (lastFramePts_ == 0.0) {
        lastFramePts_ = pts;
        frameTimer_ = av_gettime_relative() / 1000000.0;
        return 0.0;
    }
    
    // 计算当前帧与上一帧的时间差
    double diff = pts - lastFramePts_;
    if (diff <= 0 || diff >= kMaxFrameDuration) {
        // 时间戳异常，使用上一帧的持续时间
        diff = duration;
    }
    
    // 更新上一帧的PTS
    lastFramePts_ = pts;
    
    // 更新帧计时器
    double syncThreshold = std::max(kAVSyncThreshold, duration);
    double currentTime = av_gettime_relative() / 1000000.0;
    
    // 如果有主时钟，则与主时钟同步
    if (getMasterClock()) {
        double clockDiff = pts - getMasterClock()->getClock();
        
        // 视频时钟与主时钟的差异超过阈值，需要调整
        if (!std::isnan(clockDiff) && std::fabs(clockDiff) < kAVNoSyncThreshold) {
            if (clockDiff <= -syncThreshold) {
                // 视频落后于主时钟，减少延迟
                delay = 0;
            } else if (clockDiff >= syncThreshold) {
                // 视频超前于主时钟，增加延迟
                delay = 2 * duration;
            } else {
                // 在同步阈值内，正常延迟
                delay = duration;
            }
        } else {
            // 差异太大或无法计算，使用正常延迟
            delay = duration;
        }
    } else {
        // 没有主时钟，使用正常延迟
        delay = duration;
    }
    
    // 计算实际需要等待的时间
    frameTimer_ += delay;
    double actualDelay = frameTimer_ - currentTime;
    
    if (actualDelay < 0.0) {
        // 已经落后了，立即显示
        actualDelay = 0.0;
        // 重置帧计时器
        frameTimer_ = currentTime;
    }
    
    return actualDelay;
}

void SyncController::resetClocks()
{
    frameTimer_ = 0.0;
    lastFramePts_ = 0.0;
    
    if (audioClock_) {
        audioClock_->setClock(NAN, -1);
    }
    
    if (videoClock_) {
        videoClock_->setClock(NAN, -1);
    }
    
    if (externalClock_) {
        externalClock_->setClock(NAN, -1);
    }
}