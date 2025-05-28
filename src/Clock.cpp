#include <cmath>

#include "Clock.h"
#include "Utils.h"

extern "C" {
#include <libavutil/time.h>
}

namespace {
constexpr double kNoSyncThreshold = 10.0;
}

Clock::Clock()
    : pts_{0.0}, lastUpdated_{0.0}, speed_{1.0}, serial_{0}, paused_{false}
{
}

Clock::~Clock() = default;

void Clock::init(int queueSerial)
{
    std::lock_guard<std::mutex> lock(mutex_);
    speed_ = 1.0;
    paused_ = false;
    serial_ = queueSerial;
    setClock(0.0, serial_);
}

void Clock::reset()
{
    init(0);
}

double Clock::getClock() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (paused_) {
        return pts_;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        // 基准时间 + 经过时间 * 速度
        return ptsDrift_ + time - (time - lastUpdated_) * (1.0 - speed_);
    }
}

void Clock::setClock(double pts, int serial)
{
    const double time = av_gettime_relative() / 1000000.0;

    pts_ = pts;
    ptsDrift_ = pts_ - time;
    lastUpdated_ = time;
    serial_ = serial;
}

void Clock::setClockSpeed(double speed)
{
    // 确保速度为正值
    if (!utils::greater(speed, 0.0)) {
        return;
    }
    if (utils::equal(speed_, speed)) {
        return;
    }

    setClock(getClock(), serial_);
    speed_ = speed;
}

// void Clock::syncClockToSlave(const Clock& slave)
// {
//     const double clock = getClock();
//     const double slaveClock = slave.getClock();
//     if (!std::isnan(slaveClock)
//         && (std::isnan(clock) || fabs(clock - slaveClock) >
//         kNoSyncThreshold)) { setClock(slaveClock, slave.serial());
//     }
// }

void Clock::setPaused(bool paused)
{
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = paused;
}