#pragma once
#include "Clock.h"
#include "DecoderBase.h"
#include "HardwareAccel.h"
#include <memory>

class VideoDecoder : public DecoderBase 
{
public:
    VideoDecoder(std::shared_ptr<Demuxer> demuxer);
    virtual ~VideoDecoder();

    bool open() override;

    AVMediaType type() const override;
    
    // 获取视频时钟
    Clock* getClock() { return &clock_; }
    
    // 获取检测到的帧率
    double getFrameRate() const { return frameRate_; }
    
    // 设置是否启用帧率控制
    void setFrameRateControl(bool enable) { frameRateControlEnabled_ = enable; }
    
    // 获取是否启用帧率控制
    bool isFrameRateControlEnabled() const { return frameRateControlEnabled_; }

protected:
    virtual void decodeLoop() override;
    
private:
    // 更新视频帧率
    void updateFrameRate(AVRational frameRate);
    
    // 计算下一帧的显示时间
    double calculateFrameDisplayTime(double pts, double duration);

private:
    Clock clock_;                   // 视频时钟
    double frameRate_;              // 检测到的帧率
    bool frameRateControlEnabled_;  // 是否启用帧率控制
    double lastFrameTime_;          // 上一帧的显示时间

    std::shared_ptr<HardwareAccel> hwAccel_;  // 硬件加速器
};