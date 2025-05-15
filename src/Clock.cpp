#include <cmath>

#include "Clock.h"

extern "C"
{
#include <libavutil/time.h>
}

namespace {
    constexpr double kNoSyncThreshold = 10.0;
}

Clock::Clock()
    : pts_{0.0}
    , ptsDrift_{0.0}
    , lastUpdated_{0.0}
    , speed_{1.0}
    , serial_{0}
    , paused_{false}
    , queueSerial_{0}
{
}

Clock::~Clock() = default;

void Clock::init(int queueSerial)
{
    speed_ = 1.0;
    paused_ = false;
    queueSerial_ = queueSerial;
    setClock(NAN, -1);
}

double Clock::getClock() const
{
    if (queueSerial_ != serial_) {  
        return NAN;
    }

    if (paused_) {
        return pts_;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        // 基准时间 + 经过时间 * 速度
        return ptsDrift_ + lastUpdated_ + (time - lastUpdated_) * speed_;
    }
}

void Clock::setClockAt(double pts, int serial, double time)
{
    pts_ = pts;
    ptsDrift_ = pts - time;
    lastUpdated_ = time;
    serial_ = serial;
}

void Clock::setClock(double pts, int serial)
{
    const double time = av_gettime_relative() / 1000000.0;
    setClockAt(pts, serial, time);
}

void Clock::setClockSpeed(double speed)
{
    // 确保速度为正值
    if (speed <= 0.0f) {
        return;
    }

    setClock(getClock(), serial_);
    speed_ = speed;

    // Todo: 事件通知
}

void Clock::syncClockToSlave(const Clock& slave)
{
    const double clock = getClock();
    const double slaveClock = slave.getClock();
    if (!std::isnan(slaveClock) 
        && (std::isnan(clock) || fabs(clock - slaveClock) > kNoSyncThreshold)) {
        setClock(slaveClock, slave.serial());
    }
}