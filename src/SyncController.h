#pragma once
#include "Clock.h"
#include <memory>

class SyncController {
public:
    enum class MasterClock {
        Audio,
        Video,
        External
    };

    SyncController();
    ~SyncController();

    // 设置主时钟类型
    void setMasterClockType(MasterClock type);
    
    // 获取主时钟类型
    MasterClock getMasterClockType() const;
    
    // 获取主时钟
    Clock* getMasterClock();
    
    // 设置音频时钟
    void setAudioClock(Clock* clock);
    
    // 设置视频时钟
    void setVideoClock(Clock* clock);
    
    // 设置外部时钟
    void setExternalClock(Clock* clock);
    
    // 同步视频时钟到主时钟
    void syncVideoToMaster();
    
    // 同步音频时钟到主时钟
    void syncAudioToMaster();
    
    // 计算视频帧显示延迟时间
    double computeVideoDelay(double pts, double duration, double speed);
    
    // 重置所有时钟
    void resetClocks();

private:
    MasterClock masterClockType_;
    Clock* audioClock_;
    Clock* videoClock_;
    Clock* externalClock_;
    double lastFramePts_;
    double frameTimer_;
};