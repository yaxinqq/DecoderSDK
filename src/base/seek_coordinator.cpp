#include "seek_coordinator.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

SeekCoordinator::SeekCoordinator()
{
    state_.phase = SeekPhase::kIdle;
    state_.targetPosSec = -1.0;
    state_.videoSerial = 0;
    state_.audioSerial = 0;
}

void SeekCoordinator::beginSeekRequest(double positionSec)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.phase = SeekPhase::kRequested;
    state_.targetPosSec = positionSec;
    // 请求阶段暂不更新 Serial，由 Commit 阶段更新
}

void SeekCoordinator::commitSeek(double positionSec, uint64_t videoSerial, uint64_t audioSerial)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.phase = SeekPhase::kCommitted;
    state_.targetPosSec = positionSec;
    state_.videoSerial = videoSerial;
    state_.audioSerial = audioSerial;
    // 如果 Serial 为 0，说明该流不存在或不参与此次 Seek，直接标记为 Reached
    state_.videoReached = (videoSerial == 0);
    state_.audioReached = (audioSerial == 0);
}

void SeekCoordinator::failSeek(double positionSec)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.phase != SeekPhase::kIdle && state_.targetPosSec == positionSec) {
        state_.phase = SeekPhase::kIdle;
        state_.targetPosSec = -1.0;
        state_.videoReached = false;
        state_.audioReached = false;
    }
}

void SeekCoordinator::reportReachedTarget(bool isVideo, uint64_t serial)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.phase != SeekPhase::kCommitted)
        return;

    if (isVideo) {
        if (serial == state_.videoSerial)
            state_.videoReached = true;
    } else {
        if (serial == state_.audioSerial)
            state_.audioReached = true;
    }

    // 如果所有参与的流都到达了，自动结束事务
    if (state_.videoReached && state_.audioReached) {
        state_.phase = SeekPhase::kIdle;
        state_.targetPosSec = -1.0;
        // Serial 保持不变，作为基准
    }
}

SeekState SeekCoordinator::getState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool SeekCoordinator::isSeekActive() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.phase != SeekPhase::kIdle;
}

uint64_t SeekCoordinator::getCurrentVideoSerial() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.videoSerial;
}

uint64_t SeekCoordinator::getCurrentAudioSerial() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.audioSerial;
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END
