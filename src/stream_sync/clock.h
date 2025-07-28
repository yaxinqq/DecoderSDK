#ifndef DECODER_SDK_INTERNAL_CLOCK_H
#define DECODER_SDK_INTERNAL_CLOCK_H
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "base/base_define.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class Clock {
public:
    // 时钟同步类型
    enum class ClockSyncType : std::uint8_t {
        kAudioMaster,   // 音频主时钟
        kVideoMaster,   // 视频主时钟
        kExternalClock, // 外部时钟
    };

    // 时钟状态
    enum class ClockState {
        Invalid, // 无效
        Valid,   // 有效
        Stale,   // 过期
    };

public:
    /**
     * @brief 构造函数
     */
    Clock();
    /**
     * @brief 析构函数
     */
    ~Clock();

    // 禁用拷贝构造和拷贝赋值
    Clock(const Clock &) = delete;
    Clock &operator=(const Clock &) = delete;

    /**
     * @brief 初始化时钟
     * @param queueSerial 包队列数据包序号
     */
    void init(int queueSerial);
    void reset();

    /**
     * @brief 获得当前时钟
     * @return 当前时钟时间，如果时钟无效返回NaN
     */
    double getClock() const;

    /**
     * @brief 设置当前时钟（线程安全）
     * @param pts 显示时间戳
     * @param serial 数据版本号
     */
    void setClock(double pts, int serial);

    /**
     * @brief 设置当前时钟速度（线程安全）
     * @param speed 时钟速度
     */
    void setClockSpeed(double speed);

    /**
     * @brief 校准时钟，减少累积误差
     */
    void calibrate();

    /**
     * @brief 检查时钟是否有效
     */
    bool isValid() const;

    /**
     * @brief 获取时钟状态
     */
    ClockState getState() const;

    // Getter方法
    /**
     * @brief 获取当前时间戳
     */
    double pts() const;
    /**
     * @brief 获取当前时间戳与显示时间戳的差值
     */
    double ptsDrift() const;
    /**
     * @brief 获取最近一次更新时间
     */
    double lastUpdated() const;
    /**
     * @brief 获取当前时钟速度
     */
    double speed() const;
    /**
     * @brief 获取当前数据包序号
     */
    int serial() const;
    /**
     * @brief 获取当前时钟暂停状态
     */
    bool isPaused() const;

    /**
     * @brief 设置时钟的暂停状态（改进版）
     * @param paused true表示暂停，false表示播放
     */
    void setPaused(bool paused);

    /**
     * @brief 获取时钟统计信息
     */
    struct ClockStats {
        double currentTime; // 当前系统时间（秒）
        double drift;       // 时间戳与系统时间的差值（秒）
        double speed;       // 时钟速度（倍速）
        int serial;         // 当前数据包序号
        bool paused;        // 当前时钟暂停状态
        ClockState state;   // 时钟状态
    };
    /**
     * @brief 获取时钟统计信息
     */
    ClockStats getStats() const;

private:
    // 内部辅助方法
    /**
     * @brief 获取当前系统时间（秒）
     */
    double getCurrentSystemTime() const;

private:
    // 时间戳（秒）
    std::atomic<double> pts_{0.0};
    // 时间戳与系统时间的差值（秒）
    std::atomic<double> ptsDrift_{0.0};
    // 最近一次更新时间（秒）
    std::atomic<double> lastUpdated_{0.0};
    // 时钟速度（倍速）
    std::atomic<double> speed_{1.0};
    // 当前数据包序号
    std::atomic<int> serial_{0};
    // 当前时钟暂停状态
    std::atomic<bool> paused_{false};

    // 时钟校准相关
    // 校准累加器
    std::atomic<double> driftAccumulator_{0.0};
    // 校准计数器
    std::atomic<int> calibrationCounter_{0};

    mutable std::mutex mutex_; // 仅用于复杂操作

    // 常量
    static constexpr double kMaxDrift = 10.0;         // 最大允许漂移
    static constexpr int64_t kCacheValidityUs = 1000; // 缓存有效期（微秒）
    static constexpr int kCalibrationInterval = 100;  // 校准间隔
};

DECODER_SDK_NAMESPACE_END
INTERNAL_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_CLOCK_H