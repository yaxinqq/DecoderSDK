#pragma once
#include "Clock.h"
#include "DecoderBase.h"
#include "HardwareAccel.h"
#include "SyncController.h"
#include <memory>
#include <optional>

class VideoDecoder : public DecoderBase 
{
public:
    VideoDecoder(std::shared_ptr<Demuxer> demuxer, std::shared_ptr<SyncController> syncController);
    virtual ~VideoDecoder();

    bool open() override;

    AVMediaType type() const override;
    
    // 获取检测到的帧率
    double getFrameRate() const { return frameRate_; }
    
    // 设置是否启用帧率控制
    void setFrameRateControl(bool enable) { frameRateControlEnabled_ = enable; }
    
    // 获取是否启用帧率控制
    bool isFrameRateControlEnabled() const { return frameRateControlEnabled_; }

protected:
    virtual void decodeLoop() override;

    // 根据情况，是否设置解码器的硬件解码
    bool setHardwareDecode() override;
    
private:
    // 更新视频帧率
    void updateFrameRate(AVRational frameRate);
    
    // 计算下一帧的显示时间
    double calculateFrameDisplayTime(double pts, double duration);

private:
    double frameRate_;              // 检测到的帧率
    std::optional<std::chrono::steady_clock::time_point> lastFrameTime_;          // 上一帧的时间点

    std::shared_ptr<HardwareAccel> hwAccel_;  // 硬件加速器
};