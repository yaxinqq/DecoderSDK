#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

class Clock {
public:
    // 时钟同步类型
    enum class ClockSyncType : std::uint8_t {
        kAudioMaster,
        kVideoMaster,
        kExternalClock,
    };

    // 时钟状态
    enum class ClockState { Invalid, Valid, Stale };

public:
    Clock();
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
     * @brief 获得当前时钟（高性能版本）
     * @return 当前时钟时间，如果时钟无效返回NaN
     */
    double getClock() const;

    /**
     * @brief 获取时钟但不更新缓存（用于内部计算）
     */
    double getClockNoCache() const;

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

    // Getter方法（原子操作，高性能）
    double pts() const
    {
        return pts_.load(std::memory_order_acquire);
    }
    double ptsDrift() const
    {
        return ptsDrift_.load(std::memory_order_acquire);
    }
    double lastUpdated() const
    {
        return lastUpdated_.load(std::memory_order_acquire);
    }
    double speed() const
    {
        return speed_.load(std::memory_order_acquire);
    }
    int serial() const
    {
        return serial_.load(std::memory_order_acquire);
    }
    bool isPaused() const
    {
        return paused_.load(std::memory_order_acquire);
    }

    /**
     * @brief 设置时钟的暂停状态（改进版）
     * @param paused true表示暂停，false表示播放
     */
    void setPaused(bool paused);

    /**
     * @brief 获取时钟统计信息
     */
    struct ClockStats {
        double currentTime;
        double drift;
        double speed;
        int serial;
        bool paused;
        ClockState state;
        int64_t updateCount;
    };

    ClockStats getStats() const;

private:
    // 内部辅助方法
    double getCurrentSystemTime() const;
    bool isCacheValid() const;
    void updateCache(double clockValue) const;

private:
    // 使用原子变量提高性能
    std::atomic<double> pts_{0.0};
    std::atomic<double> ptsDrift_{0.0};
    std::atomic<double> lastUpdated_{0.0};
    std::atomic<double> speed_{1.0};
    std::atomic<int> serial_{0};
    std::atomic<bool> paused_{false};

    // 性能优化相关
    mutable std::atomic<double> cachedClock_{0.0};
    mutable std::atomic<int64_t> cacheTimestamp_{0};
    mutable std::atomic<int64_t> updateCount_{0};

    // 时钟校准相关
    std::atomic<double> driftAccumulator_{0.0};
    std::atomic<int> calibrationCounter_{0};

    mutable std::mutex mutex_;  // 仅用于复杂操作

    // 常量
    static constexpr double kMaxDrift = 10.0;          // 最大允许漂移
    static constexpr int64_t kCacheValidityUs = 1000;  // 缓存有效期（微秒）
    static constexpr int kCalibrationInterval = 100;   // 校准间隔
};