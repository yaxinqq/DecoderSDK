#pragma once
#include <cstdint>
#include <mutex>

class Clock
{
public:
    enum class ClockSyncType: std::uint8_t {
        kAudioMaster,
        kVideoMaster,
        kExternalClock,
    };

public:
    Clock();
    ~Clock();

    /*
     * @brief 初始化时钟
     * @param queueSerial 包队列数据包序号
     */
    void init(int queueSerial);
    void reset();

    /*
     * @brief 获得当前时钟
     */
    double getClock() const;

    /*
     * @brief 设置当前时钟
     * @param pts 显示时间戳
     * @param serial 数据版本号
     */
    void setClock(double pts, int serial);
    /*
     * @brief 设置当前时钟速度
     * @param speed 时钟速度
     */
    void setClockSpeed(double speed);

    /*
     * @brief 和其他时钟进行同步
     * @param slave 从时钟
     */
    // void syncClockToSlave(const Clock& slave);

    /*
     * @brief 获取当前显示时间戳（PTS）
     * @return 当前显示时间戳
     */
    double pts() const { return pts_; }
	
	/*
     * @brief 获取PTS相对于系统时间的漂移值
     * @return PTS漂移值
     */
    double ptsDrift() const { return ptsDrift_; }
	
    /*
     * @brief 获取上次更新的系统时间
     * @return 上次更新时间
     */
    double lastUpdated() const { return lastUpdated_; }

    /*
     * @brief 获取时钟播放速度
     * @return 播放速度（如2.0表示2倍速）
     */
    double speed() const { return speed_; }

    /*
     * @brief 获取时钟对应的数据包序号
     * @return 数据包序号
     */
    int serial() const { return serial_; }

    /*
     * @brief 获取时钟是否处于暂停状态
     * @return true表示暂停，false表示正在播放
     */
    bool isPaused() const { return paused_; }

    /*
     * @brief 设置时钟的暂停状态
     * @param paused true表示暂停，false表示播放
     */
    void setPaused(bool paused);

private:
    mutable std::mutex mutex_;               // 互斥锁，用于保护共享资源的访问

    double pts_;                    // 当前时钟的显示时间（Presentation Time Stamp）
    double ptsDrift_;               // pts - 当前系统时间，表示相对于系统时间的漂移 
    double lastUpdated_;            // 上次更新时间（系统时间），用于计算经过的时间
    double speed_;                  // 时钟的播放速度，比如倍速播放时为 2.0
    int serial_;                    // 该时钟对应数据包的序号（比如解码序列），用于判断是否为最新数据
    bool paused_;                   // 是否暂停，1 为暂停
};
