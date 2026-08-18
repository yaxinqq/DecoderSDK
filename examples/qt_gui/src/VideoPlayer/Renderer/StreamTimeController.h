#pragma once
#include "../Base/CommonDef.h"

#include "decodersdk/common_define.h"
#include "decodersdk/frame.h"

#include <chrono>
#include <cstdint>
#include <memory>

/**
 * @brief Renderer 私有时间控制器
 *
 * 职责：
 * 1. 管理音频、视频、外部时钟的基准点与推进逻辑
 * 2. 统一管理播放倍速与暂停/恢复状态
 * 3. 基于当前主时钟，为视频帧提供调度决策（显示、等待、丢弃）
 */
class StreamTimeController {
public:
    StreamTimeController();
    ~StreamTimeController();

    /**
     * @brief 重置全部状态（Stop 或重新 Start 后调用）
     * 
     * @param 是否跳过重置倍速
     */
    void reset(bool skipResetSpeed = true);

    /**
     * @brief 设置主时钟源类型
     */
    void setClockSourceType(Stream::ClockSourceType type);

    /**
     * @brief 设置播放倍速
     */
    void setSpeed(double speed);

    /**
     * @brief 设置音频时钟基准（由 AudioRender 在首帧渲染时调用）
     * @param basePts 首帧音频 PTS
     * @param baseHardwareUSecs 此时设备已播放的微秒数
     */
    void setAudioClockBase(double basePts, int64_t baseHardwareUSecs);

    /**
     * @brief 更新音频硬件进度（由 Renderer 定期采样并更新）
     * @param currentHardwareUSecs 当前设备已播放的微秒数
     */
    void updateAudioHardwareProgress(int64_t currentHardwareUSecs);

    /**
     * @brief 视频渲染成功回调，更新视频时钟
     * @param videoPts 已成功显示的视频 PTS
     */
    void onVideoRendered(double videoPts);

    /**
     * @brief 判定视频帧的调度行为
     * @param videoPts 待调度的视频帧 PTS
     * @return 调度决策结果
     */
    Stream::VideoScheduleDecision decideVideo(double videoPts) const;

    /**
     * @brief 判定音频帧的调度行为
     * @param audioPts 待调度的音频帧 PTS
     * @param bytesFree 音频硬件缓冲区剩余空间
     * @return 调度决策结果
     */
    Stream::AudioScheduleDecision decideAudio(double audioPts, int bytesFree) const;

    /**
     * @brief 获取当前主时钟的值（秒）
     */
    double getMasterClock() const;

    /**
     * @brief 设置同步配置
     */
    void setSyncOptions(const Stream::PlaybackSyncOptions &options);

private:
    /**
     * @brief 获取系统单调时钟时间（秒）
     */
    double getSteadyTimeSec() const;

    /**
     * @brief 更新外部时钟基准点
     */
    void updateExternalClockBase(double newPts);

private:
    // 配置参数
    Stream::ClockSourceType clockSourceType_ = Stream::ClockSourceType::kNone;
    Stream::PlaybackSyncOptions syncOptions_;

    // 运行态标识
    bool isPaused_ = false;
    double speed_ = 1.0;

    // 三类时钟的基础状态
    struct ClockState {
        double basePts = 0.0;
        double baseSteadyTime = 0.0;
        int64_t baseHardwareUSecs = 0;    // 音频硬件基准微秒
        int64_t currentHardwareUSecs = 0; // 当前采样到的硬件微秒
        bool valid = false;
    };

    ClockState audioClock_;
    ClockState videoClock_;
    ClockState externalClock_;
};
