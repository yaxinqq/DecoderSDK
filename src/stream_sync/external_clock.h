#ifndef DECODER_SDK_INTERNAL_EXTERNAL_CLOCK_H
#define DECODER_SDK_INTERNAL_EXTERNAL_CLOCK_H

#include <atomic>
#include <mutex>

#include "base/base_define.h"

extern "C" {
#include <libavutil/time.h>
}

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

/**
 * @brief 简洁的外部时钟 - 全局参考系
 *
 * 核心公式：current_clock = base_pts + (now - base_time) * speed
 */
class ExternalClock {
public:
    ExternalClock();
    ~ExternalClock() = default;

    // 禁用拷贝
    ExternalClock(const ExternalClock &) = delete;
    ExternalClock &operator=(const ExternalClock &) = delete;

    /**
     * @brief 获取当前时钟值（秒）
     * @return 当前时钟时间，与AVFrame/AVPacket的pts/dts在同一时间基准
     */
    double getClock() const;

    /**
     * @brief 设置播放速度
     * @param speed 播放速度（1.0为正常速度）
     */
    void setSpeed(double speed);

    /**
     * @brief 获取当前播放速度
     */
    double getSpeed() const;

    /**
     * @brief 跳转到指定时间点
     * @param targetPts 目标时间点（秒）
     */
    void seekTo(double targetPts);

    /**
     * @brief 暂停/恢复
     * @param paused true=暂停，false=恢复
     */
    void setPaused(bool paused);

    /**
     * @brief 获取暂停状态
     */
    bool isPaused() const;

    /**
     * @brief 重置时钟
     */
    void reset();

private:
    /**
     * @brief 获取当前系统时间（秒）
     */
    double getCurrentSystemTime() const;

    /**
     * @brief 更新基准点（线程安全）
     */
    void updateBasePoint(double pts);

private:
    mutable std::mutex mutex_;

    // 核心状态：current_clock = base_pts + (now - base_time) * speed
    std::atomic<double> basePts_{0.0};  // 基准PTS（秒）
    std::atomic<double> baseTime_{0.0}; // 基准系统时间（秒）
    std::atomic<double> speed_{1.0};    // 播放速度
    std::atomic<bool> paused_{false};   // 暂停状态
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_EXTERNAL_CLOCK_H