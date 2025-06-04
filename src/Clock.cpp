#include "Clock.h"
#include "Utils.h"
#include <algorithm>
#include <cmath>

extern "C" {
#include <libavutil/time.h>
}

Clock::Clock() = default;
Clock::~Clock() = default;

void Clock::init(int queueSerial)
{
    speed_.store(1.0, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    serial_.store(queueSerial, std::memory_order_release);
    updateCount_.store(0, std::memory_order_release);
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
    // 快速路径：检查缓存是否有效
    if (isCacheValid()) {
        return cachedClock_.load(std::memory_order_acquire);
    }

    // 慢速路径：重新计算
    double clockValue = getClockNoCache();
    updateCache(clockValue);

    return clockValue;
}

double Clock::getClockNoCache() const
{
    if (paused_.load(std::memory_order_acquire)) {
        return pts_.load(std::memory_order_acquire);
    }

    double currentTime = getCurrentSystemTime();
    double lastUpdate = lastUpdated_.load(std::memory_order_acquire);
    double drift = ptsDrift_.load(std::memory_order_acquire);
    double speed = speed_.load(std::memory_order_acquire);

    // 改进的时钟计算公式，减少累积误差
    double elapsed = currentTime - lastUpdate;
    return drift + currentTime - elapsed * (1.0 - speed);
}

void Clock::setClock(double pts, int serial)
{
    double currentTime = getCurrentSystemTime();

    pts_.store(pts, std::memory_order_release);
    ptsDrift_.store(pts - currentTime, std::memory_order_release);
    lastUpdated_.store(currentTime, std::memory_order_release);
    serial_.store(serial, std::memory_order_release);
    updateCount_.fetch_add(1, std::memory_order_acq_rel);

    // 清除缓存
    cacheTimestamp_.store(0, std::memory_order_release);
}

void Clock::setClockSpeed(double speed)
{
    if (!utils::greater(speed, 0.0) || utils::equal(speed_.load(), speed)) {
        return;
    }

    // 线程安全的速度更新
    double currentClock = getClockNoCache();
    int currentSerial = serial_.load(std::memory_order_acquire);

    speed_.store(speed, std::memory_order_release);
    setClock(currentClock, currentSerial);
}

void Clock::calibrate()
{
    int counter = calibrationCounter_.fetch_add(1, std::memory_order_acq_rel);
    if (counter % kCalibrationInterval != 0) {
        return;
    }

    // 检查并修正累积漂移
    double drift = ptsDrift_.load(std::memory_order_acquire);
    if (std::abs(drift) > kMaxDrift) {
        std::lock_guard<std::mutex> lock(mutex_);
        double currentClock = getClockNoCache();
        setClock(currentClock, serial_.load());
    }
}

bool Clock::isValid() const
{
    return updateCount_.load(std::memory_order_acquire) > 0 &&
           !std::isnan(pts_.load(std::memory_order_acquire));
}

Clock::ClockState Clock::getState() const
{
    if (!isValid()) {
        return ClockState::Invalid;
    }

    double currentTime = getCurrentSystemTime();
    double lastUpdate = lastUpdated_.load(std::memory_order_acquire);

    // 如果超过5秒没有更新，认为时钟过期
    if (currentTime - lastUpdate > 5.0) {
        return ClockState::Stale;
    }

    return ClockState::Valid;
}

void Clock::setPaused(bool paused)
{
    bool wasPaused = paused_.load(std::memory_order_acquire);
    if (wasPaused == paused) {
        return;
    }

    if (paused) {
        // 暂停：记录当前时钟值
        double currentClock = getClockNoCache();
        pts_.store(currentClock, std::memory_order_release);
    } else {
        // 恢复：重新设置时钟
        double currentPts = pts_.load(std::memory_order_acquire);
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
            getState(),
            updateCount_.load(std::memory_order_acquire)};
}

// 私有方法实现
double Clock::getCurrentSystemTime() const
{
    return av_gettime_relative() / 1000000.0;
}

bool Clock::isCacheValid() const
{
    int64_t currentTime = av_gettime_relative();
    int64_t cacheTime = cacheTimestamp_.load(std::memory_order_acquire);
    return (currentTime - cacheTime) < kCacheValidityUs;
}

void Clock::updateCache(double clockValue) const
{
    cachedClock_.store(clockValue, std::memory_order_release);
    cacheTimestamp_.store(av_gettime_relative(), std::memory_order_release);
}