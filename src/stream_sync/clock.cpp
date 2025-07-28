#include "clock.h"

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavutil/time.h>
}

#include "utils/common_utils.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

Clock::Clock() = default;
Clock::~Clock() = default;

void Clock::init(int queueSerial)
{
    speed_.store(1.0, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    serial_.store(queueSerial, std::memory_order_release);
    driftAccumulator_.store(0.0, std::memory_order_release);
    calibrationCounter_.store(0, std::memory_order_release);

    setClock(0.0, queueSerial);
}

void Clock::reset()
{
    init(0);
}

double Clock::getClock() const
{
    if (paused_.load(std::memory_order_acquire)) {
        return pts_.load(std::memory_order_acquire);
    }

    const double currentTime = getCurrentSystemTime();
    const double lastUpdate = lastUpdated_.load(std::memory_order_acquire);
    const double drift = ptsDrift_.load(std::memory_order_acquire);
    const double speed = speed_.load(std::memory_order_acquire);

    // 时钟计算公式
    const double elapsed = (currentTime - lastUpdate) * speed;
    return drift + currentTime - elapsed;
}

void Clock::setClock(double pts, int serial)
{
    const double currentTime = getCurrentSystemTime();

    // 限制漂移范围，防止漂移过大
    const double drift = pts - currentTime;

    pts_.store(pts, std::memory_order_release);
    ptsDrift_.store(drift, std::memory_order_release);
    lastUpdated_.store(currentTime, std::memory_order_release);
    serial_.store(serial, std::memory_order_release);
}

void Clock::setClockSpeed(double speed)
{
    if (!utils::greater(speed, 0.0) || utils::equal(speed_.load(), speed)) {
        return;
    }

    speed_.store(speed, std::memory_order_release);

    // 线程安全的速度更新
    const double currentClock = getClock();
    const int currentSerial = serial_.load(std::memory_order_acquire);

    setClock(currentClock, currentSerial);
}

void Clock::calibrate()
{
    const int counter = calibrationCounter_.fetch_add(1, std::memory_order_acq_rel);
    if (counter % kCalibrationInterval != 0) {
        return;
    }

    // 检查并修正累积漂移
    const double drift = ptsDrift_.load(std::memory_order_acquire);
    if (std::abs(drift) > kMaxDrift) {
        std::lock_guard<std::mutex> lock(mutex_);
        const double currentClock = getClock();
        setClock(currentClock, serial_.load());
    }
}

bool Clock::isValid() const
{
    return !std::isnan(pts_.load(std::memory_order_acquire));
}

Clock::ClockState Clock::getState() const
{
    if (!isValid()) {
        return ClockState::Invalid;
    }

    const double currentTime = getCurrentSystemTime();
    const double lastUpdate = lastUpdated_.load(std::memory_order_acquire);

    // 如果超过5秒没有更新，认为时钟过期
    if (currentTime - lastUpdate > 5.0) {
        return ClockState::Stale;
    }

    return ClockState::Valid;
}

double Clock::pts() const
{
    return pts_.load(std::memory_order_acquire);
}

double Clock::ptsDrift() const
{
    return ptsDrift_.load(std::memory_order_acquire);
}

double Clock::lastUpdated() const
{
    return lastUpdated_.load(std::memory_order_acquire);
}

double Clock::speed() const
{
    return speed_.load(std::memory_order_acquire);
}

int Clock::serial() const
{
    return serial_.load(std::memory_order_acquire);
}

bool Clock::isPaused() const
{
    return paused_.load(std::memory_order_acquire);
}

void Clock::setPaused(bool paused)
{
    const bool wasPaused = paused_.load(std::memory_order_acquire);
    if (wasPaused == paused) {
        return;
    }

    if (paused) {
        // 暂停：记录当前时钟值
        const double currentClock = getClock();
        pts_.store(currentClock, std::memory_order_release);
    } else {
        // 恢复：重新设置时钟
        const double currentPts = pts_.load(std::memory_order_acquire);
        setClock(currentPts, serial_.load());
    }

    paused_.store(paused, std::memory_order_release);
}

Clock::ClockStats Clock::getStats() const
{
    return {getClock(),
            ptsDrift_.load(std::memory_order_acquire),
            speed_.load(std::memory_order_acquire),
            serial_.load(std::memory_order_acquire),
            paused_.load(std::memory_order_acquire),
            getState()};
}

double Clock::getCurrentSystemTime() const
{
    return av_gettime_relative() / 1000000.0;
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END