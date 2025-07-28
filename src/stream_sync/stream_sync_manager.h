#ifndef DECODER_SDK_INTERNAL_STREAM_SYNC_MANAGER_H
#define DECODER_SDK_INTERNAL_STREAM_SYNC_MANAGER_H
#include <atomic>
#include <chrono>
#include <memory>

#include "base/base_define.h"
#include "clock.h"
#include "include/decodersdk/common_define.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class StreamSyncManager {
public:
    /**
     * @brief 构造一个新的 Stream Sync Manager 对象
     *
     * @param type 主时钟类型
     * @param avSyncThreshold 音视频同步阈值
     * @param avSyncMaxDrift 音视频最大同步漂移
     * @param jitterAlpha 抖动平滑系数
     */
    StreamSyncManager(MasterClock type = MasterClock::kVideo,
                      double avSyncThreshold = 0.010, // 10 ms
                      double avSyncMaxDrift = 0.100,  // 100 ms
                      double jitterAlpha = 0.1);      // 抖动平滑系数

    /**
     * @brief 设置主时钟类型
     *
     * @param m 主时钟类型
     */
    void setMaster(MasterClock m);
    /**
     * @brief 获取主时钟类型
     *
     * @return MasterClock 主时钟类型
     */
    MasterClock master() const;

    /**
     * @brief 设置音视频同步阈值
     *
     * @param threshold 阈值（秒）
     */
    void setAVSyncThreshold(double threshold);

    /**
     * @brief 设置是否自适应同步
     * @param enable 是否开启
     */
    void setAdaptiveSync(bool enable);

    /**
     * @brief 获得当前主时钟时间（单位：秒）
     * @return double
     */
    double getMasterClock() const;
    /**
     * @brief 设置主时钟速度
     * @param speed 速度（倍速）
     */
    void setSpeed(double speed);

    /**
     * @brief 更新音频时钟
     * @param pts 时间戳
     * @param serial 序列号
     */
    void updateAudioClock(double pts, int serial = 0);
    /**
     * @brief 更新视频时钟
     * @param pts 时间戳
     * @param serial 序列号
     */
    void updateVideoClock(double pts, int serial = 0);
    /**
     * @brief 更新外部时钟
     * @param pts 时间戳
     * @param serial 序列号
     */
    void updateExternalClock(double pts, int serial = 0);
    /**
     * @brief 重置所有时钟
     */
    void resetClocks();

    /**
     * @brief 计算视频延迟
     * @param framePts 视频时间戳
     * @param frameDuration 视频帧时长
     * @param baseDelay 基础延迟
     * @param speed 速度（倍速）
     * @return double 视频延迟（秒）
     */
    double computeVideoDelay(double framePts, double frameDuration, double baseDelay, double speed);
    /**
     * @brief 计算音频延迟
     * @param audioPts 音频时间戳
     * @param bufferDelay 缓冲延迟
     * @param speed 速度（倍速）
     * @return double 音频延迟（秒）
     */
    double computeAudioDelay(double audioPts, double bufferDelay, double speed);

    /**
     * @brief 是否应该丢弃当前帧
     * @param framePts 视频时间戳
     * @param frameDuration 视频帧时长
     * @return true - 丢弃；false - 不丢弃
     */
    bool shouldDropFrame(double framePts, double frameDuration) const;
    /**
     * @brief 是否应该重复当前帧
     * @param framePts 视频时间戳
     * @param frameDuration 视频帧时长
     * @return true - 重复；false - 不重复
     */
    bool shouldDuplicateFrame(double framePts, double frameDuration) const;

    /**
     * @brief 获取同步状态
     * @return SyncState 同步状态
     */
    SyncState getSyncState() const;
    /**
     * @brief 获取同步统计信息
     * @return SyncStats 同步统计信息
     */
    SyncStats getStats() const;

    /**
     * @brief 更新同步参数
     */
    void updateSyncParameters();

    /**
     * @brief 获取同步质量统计
     * @return SyncQualityStats 同步质量统计信息
     */
    SyncQualityStats getSyncQualityStats() const;

private:
    // 内部方法
    /**
     * @brief 获取缓存的主时钟时间
     * @return double 缓存的主时钟时间（秒）
     */
    double getMasterClockCached() const;
    /**
     * @brief 更新同步质量统计
     * @param drift 同步漂移（秒）
     */
    void updateSyncQuality(double drift);
    /**
     * @brief 计算自适应阈值
     * @return double 自适应阈值（秒）
     */
    double computeAdaptiveThreshold() const;
    /**
     * @brief 评估同步状态
     * @param drift 同步漂移（秒）
     * @return SyncState 同步状态
     */
    SyncState evaluateSyncState(double drift) const;

private:
    std::atomic<MasterClock> master_; // 主时钟类型
    Clock audioClock_;                // 音频时钟
    Clock videoClock_;                // 视频时钟
    Clock externalClock_;             // 外部时钟

    // 同步参数
    std::atomic<double> syncThreshold_{0.0}; // 音视频同步阈值
    std::atomic<double> maxDrift_{0.0};      // 音视频最大同步漂移
    std::atomic<double> alpha_{0.0};         // 抖动平滑系数
    std::atomic<bool> adaptiveSync_{true};   // 是否自适应同步

    // 同步质量统计
    std::atomic<int> goodSyncCount_{0};   // 同步质量好的次数
    std::atomic<int> poorSyncCount_{0};   // 同步质量差的次数
    std::atomic<double> totalDrift_{0.0}; // 总同步漂移
    std::atomic<double> avgDrift_{0.0};   // 平均同步漂移

    // 平滑漂移值
    std::atomic<double> smoothedVideoDrift_{0.0}; // 视频平滑漂移
    std::atomic<double> smoothedAudioDrift_{0.0}; // 音频平滑漂移

    // 统计信息
    std::atomic<int> droppedFrames_{0};      // 丢帧数
    std::atomic<int> duplicatedFrames_{0};   // 重复帧数
    std::atomic<double> avgVideoDelay_{0.0}; // 视频平均延迟
    std::atomic<double> avgAudioDelay_{0.0}; // 音频平均延迟

    // 自适应参数
    std::atomic<int> syncQualityCounter_{0};       // 同步质量计数器
    std::atomic<double> adaptiveThreshold_{0.010}; // 自适应阈值（秒）
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_STREAM_SYNC_MANAGER_H