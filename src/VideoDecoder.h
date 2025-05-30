#pragma once
#include "Clock.h"
#include "DecoderBase.h"
#include "HardwareAccel.h"
#include "SyncController.h"
#include <memory>
#include <optional>

class VideoDecoder : public DecoderBase {
public:
    VideoDecoder(std::shared_ptr<Demuxer> demuxer,
                 std::shared_ptr<SyncController> syncController,
                 std::shared_ptr<EventDispatcher> eventDispatcher);
    virtual ~VideoDecoder();

    void init(HWAccelType type = HWAccelType::AUTO, int deviceIndex = 0,
              AVPixelFormat softPixelFormat = AV_PIX_FMT_YUV420P,
              bool requireFrameInMemory = false);

    bool open() override;

    AVMediaType type() const override;

    // 需要解码后的帧位于内存中
    bool requireFrameInSystemMemory(bool required = true);

    // 获取检测到的帧率
    double getFrameRate() const
    {
        return frameRate_;
    }

    // 设置是否启用帧率控制
    void setFrameRateControl(bool enable)
    {
        frameRateControlEnabled_ = enable;
    }

    // 获取是否启用帧率控制
    bool isFrameRateControlEnabled() const
    {
        return frameRateControlEnabled_;
    }

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
    double frameRate_;  // 检测到的帧率
    std::optional<std::chrono::steady_clock::time_point>
        lastFrameTime_;  // 上一帧的时间点

    std::shared_ptr<HardwareAccel> hwAccel_;  // 硬件加速器
    // 当前指定的硬件加速类型
    HWAccelType hwAccelType_ = HWAccelType::AUTO;
    // 当前指定的加速硬件
    int deviceIndex_ = 0;

    // 软解时的图像格式
    AVPixelFormat softPixelFormat_ = AV_PIX_FMT_YUV420P;

    // 需要解码后的帧位于内存中
    bool requireFrameInMemory_ = false;
};