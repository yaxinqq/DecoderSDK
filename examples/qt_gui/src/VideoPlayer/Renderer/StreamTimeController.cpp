#include "StreamTimeController.h"

#include <QDebug>

#include <algorithm>
#include <cmath>

StreamTimeController::StreamTimeController()
{
    reset();
}

StreamTimeController::~StreamTimeController()
{
}

void StreamTimeController::reset(bool skipResetSpeed)
{
    isPaused_ = false;

    if (!skipResetSpeed) {
        speed_ = 1.0;
    }

    audioClock_ = {0.0, 0.0, 0, 0, false};
    videoClock_ = {0.0, 0.0, 0, 0, false};
    externalClock_ = {0.0, 0.0, 0, 0, false};
}

void StreamTimeController::setClockSourceType(Stream::ClockSourceType type)
{
    clockSourceType_ = type;
}

void StreamTimeController::setSpeed(double speed)
{
    if (std::abs(speed_ - speed) < 0.0001) {
        return;
    }

    // 切速原则：先固定当前流时间，再以新速度继续推进
    double currentPts = getMasterClock();
    speed_ = speed;

    // 必须更新当前主时钟的基准，否则 getMasterClock 计算 elapsed * speed_ 时会因为 speed_
    // 变化而产生跳变
    switch (clockSourceType_) {
        case Stream::ClockSourceType::kAudioMaster:
            if (audioClock_.valid) {
                audioClock_.basePts = currentPts;
                audioClock_.baseHardwareUSecs = audioClock_.currentHardwareUSecs;
            }
            break;
        case Stream::ClockSourceType::kVideoMaster:
            if (videoClock_.valid) {
                videoClock_.basePts = currentPts;
                videoClock_.baseSteadyTime = getSteadyTimeSec();
            }
            break;
        case Stream::ClockSourceType::kExternalMaster:
            updateExternalClockBase(currentPts);
            break;
        default:
            break;
    }
}

void StreamTimeController::setAudioClockBase(double basePts, int64_t baseHardwareUSecs)
{
    audioClock_.basePts = basePts;
    audioClock_.baseHardwareUSecs = baseHardwareUSecs;
    audioClock_.currentHardwareUSecs = baseHardwareUSecs;
    audioClock_.valid = true;
}

void StreamTimeController::updateAudioHardwareProgress(int64_t currentHardwareUSecs)
{
    if (audioClock_.valid) {
        audioClock_.currentHardwareUSecs = currentHardwareUSecs;
    }
}

void StreamTimeController::onVideoRendered(double videoPts)
{
    videoClock_.basePts = videoPts;
    videoClock_.baseSteadyTime = getSteadyTimeSec();
    videoClock_.valid = true;

    // 如果是视频主时钟或外部时钟，且对应的基准尚未建立，则同步建立基准
    if ((clockSourceType_ == Stream::ClockSourceType::kVideoMaster ||
         clockSourceType_ == Stream::ClockSourceType::kExternalMaster) &&
        !externalClock_.valid) {
        updateExternalClockBase(videoPts);
        externalClock_.valid = true;
    }
}

Stream::VideoScheduleDecision StreamTimeController::decideVideo(double videoPts) const
{
    Stream::VideoScheduleDecision decision;

    // 无脑渲染模式：不进行任何时间检查，直接渲染
    if (clockSourceType_ == Stream::ClockSourceType::kNone) {
        decision.action = Stream::ScheduleAction::kRenderNow;
        return decision;
    }

    // 1. 检查主时钟是否就绪
    bool masterReady = false;
    switch (clockSourceType_) {
        case Stream::ClockSourceType::kAudioMaster:
            masterReady = audioClock_.valid;
            break;
        case Stream::ClockSourceType::kVideoMaster:
            // 视频主时钟模式下，第一帧总是立即显示以建立基准
            if (!videoClock_.valid) {
                decision.action = Stream::ScheduleAction::kRenderNow;
                return decision;
            }
            masterReady = true;
            break;
        case Stream::ClockSourceType::kExternalMaster:
            if (!externalClock_.valid) {
                // 外部时钟模式下，若尚未建立基准，则第一帧立即显示以建立基准
                decision.action = Stream::ScheduleAction::kRenderNow;
                return decision;
            }
            masterReady = true;
            break;
        default:
            break;
    }

    if (!masterReady) {
        decision.action = Stream::ScheduleAction::kNoClockReady;
        return decision;
    }

    // 2. 计算同步偏差
    double masterClock = getMasterClock();
    double diff = videoPts - masterClock;

    // 3. 执行调度决策
    if (diff > syncOptions_.syncThresholdSec) {
        // 视频超前过多 -> 等待
        decision.action = Stream::ScheduleAction::kWait;
    } else if (diff < -syncOptions_.dropThresholdSec) {
        // 视频落后过多 -> 丢弃
        decision.action = Stream::ScheduleAction::kDrop;
    } else {
        // 落在同步窗口内 -> 立即渲染
        decision.action = Stream::ScheduleAction::kRenderNow;
    }

    return decision;
}

Stream::AudioScheduleDecision StreamTimeController::decideAudio(double audioPts,
                                                                int bytesFree) const
{
    Stream::AudioScheduleDecision decision;

    // 1. 无脑渲染模式：不进行任何时间检查，直接渲染
    if (clockSourceType_ == Stream::ClockSourceType::kNone) {
        decision.action = Stream::ScheduleAction::kRenderNow;
        return decision;
    }

    // 2. 检查硬件缓冲区空间
    // 如果剩余空间极小（例如不足 4KB，约等于 1024 采样 16bit 双声道的空间），则进入等待
    // 这样可以防止在 AudioMaster 模式下无脑 Pop 帧导致的数据积压或无效尝试
    if (bytesFree < 4096 && audioClock_.valid) {
        decision.action = Stream::ScheduleAction::kWait;
        return decision;
    }

    // 3. 如果是音频主控模式，且空间足够，为了让音频驱动时钟，必须尽快送入音频
    if (clockSourceType_ == Stream::ClockSourceType::kAudioMaster) {
        decision.action = Stream::ScheduleAction::kRenderNow;
        return decision;
    }

    // 4. 检查主时钟是否就绪（对于视频主控或外部主控）
    bool masterReady = false;
    switch (clockSourceType_) {
        case Stream::ClockSourceType::kVideoMaster:
            masterReady = videoClock_.valid;
            break;
        case Stream::ClockSourceType::kExternalMaster:
            masterReady = externalClock_.valid;
            break;
        default:
            break;
    }

    if (!masterReady) {
        decision.action = Stream::ScheduleAction::kNoClockReady;
        return decision;
    }

    // 5. 计算同步偏差
    double masterClock = getMasterClock();
    double diff = audioPts - masterClock;

    // 音频同步策略：
    // - 允许音频有一定程度的超前（用于填充硬件缓冲区），暂定 100ms
    const double kAudioAheadThreshold = 0.1;

    if (diff > kAudioAheadThreshold) {
        decision.action = Stream::ScheduleAction::kWait;
    } else {
        decision.action = Stream::ScheduleAction::kRenderNow;
    }

    return decision;
}

double StreamTimeController::getMasterClock() const
{
    switch (clockSourceType_) {
        case Stream::ClockSourceType::kNone:
            // 无脑渲染模式下没有主时钟概念，返回 0 即可
            return 0.0;
        case Stream::ClockSourceType::kAudioMaster:
            if (audioClock_.valid) {
                // 音频时钟：以 AudioRender 首帧建立的基准为准，按硬件播放进度推进
                // 由于解码器已经根据倍速进行了重采样，音频硬件播放 1s 数据即代表流时间推进了 1s *
                // speed_ 因此计算逻辑保持不变，依然是 elapsed * speed_，但需要确保逻辑上不冲突
                double elapsed =
                    (audioClock_.currentHardwareUSecs - audioClock_.baseHardwareUSecs) / 1000000.0;
                return audioClock_.basePts + elapsed * speed_;
            }
            break;
        case Stream::ClockSourceType::kVideoMaster:
            if (videoClock_.valid) {
                // 视频主时钟：以最后显示的 PTS 为基准，按单调时钟和倍速推进
                double elapsed = getSteadyTimeSec() - videoClock_.baseSteadyTime;
                return videoClock_.basePts + elapsed * speed_;
            }
            break;
        case Stream::ClockSourceType::kExternalMaster:
            if (externalClock_.valid) {
                // 外部时钟：按单调时钟线性推进
                double elapsed = getSteadyTimeSec() - externalClock_.baseSteadyTime;
                return externalClock_.basePts + elapsed * speed_;
            }
            break;
    }

    return 0.0;
}

void StreamTimeController::setSyncOptions(const Stream::PlaybackSyncOptions &options)
{
    syncOptions_ = options;
}

double StreamTimeController::getSteadyTimeSec() const
{
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count() / 1000000.0;
}

void StreamTimeController::updateExternalClockBase(double newPts)
{
    externalClock_.basePts = newPts;
    externalClock_.baseSteadyTime = getSteadyTimeSec();
    externalClock_.valid = true;
}
