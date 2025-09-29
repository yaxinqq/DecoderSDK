#include "external_clock.h"
#include "utils/common_utils.h"

#include "logger/logger.h"

#include <algorithm>

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

ExternalClock::ExternalClock()
{
    reset();
}

double ExternalClock::getClock() const
{
    if (paused_.load(std::memory_order_acquire)) {
        return basePts_.load(std::memory_order_acquire);
    }

    // 计算当前时钟值：current_clock = base_pts + (now - base_time) * speed
    const double now = getCurrentSystemTime();
    const double basePts = basePts_.load(std::memory_order_acquire);
    const double baseTime = baseTime_.load(std::memory_order_acquire);
    const double speed = speed_.load(std::memory_order_acquire);

    return basePts + (now - baseTime) * speed;
}

void ExternalClock::setSpeed(double speed)
{
    if (!utils::greater(speed, 0.0) || utils::equal(speed, speed_.load())) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 更新基准点，保持时钟连续性
    const double currentClock = getClock();
    updateBasePoint(currentClock);

    speed_.store(speed, std::memory_order_release);
}

double ExternalClock::getSpeed() const
{
    return speed_.load(std::memory_order_acquire);
}

void ExternalClock::seekTo(double targetPts)
{
    std::lock_guard<std::mutex> lock(mutex_);
    updateBasePoint(targetPts);
}

void ExternalClock::setPaused(bool paused)
{
    const bool wasPaused = paused_.load(std::memory_order_acquire);
    if (wasPaused == paused) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (paused) {
        // 暂停：记录当前时钟值作为基准PTS
        const double currentClock = getClock();
        basePts_.store(currentClock, std::memory_order_release);
    } else {
        // 恢复：重新设置基准时间点
        const double currentPts = basePts_.load(std::memory_order_acquire);
        updateBasePoint(currentPts);
    }

    paused_.store(paused, std::memory_order_release);
}

bool ExternalClock::isPaused() const
{
    return paused_.load(std::memory_order_acquire);
}

void ExternalClock::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);

    const double now = getCurrentSystemTime();
    basePts_.store(0.0, std::memory_order_release);
    baseTime_.store(now, std::memory_order_release);
    speed_.store(1.0, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
}

double ExternalClock::getCurrentSystemTime() const
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void ExternalClock::updateBasePoint(double pts)
{
    const double now = getCurrentSystemTime();
    basePts_.store(pts, std::memory_order_release);
    baseTime_.store(now, std::memory_order_release);
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END