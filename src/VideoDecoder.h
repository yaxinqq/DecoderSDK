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

protected:
    void decodeLoop() override;
    bool setHardwareDecode() override;

private:
    // 更新视频帧率
    void updateFrameRate(AVRational frameRate);

    // 处理帧格式转换
    Frame processFrameConversion(const Frame &inputFrame);

    // 处理硬件帧到内存的转换
    Frame transferHardwareFrame(const Frame &hwFrame);

    // 处理软件帧格式转换
    Frame convertSoftwareFrame(const Frame &frame);

private:
    double frameRate_;  // 检测到的帧率
    std::optional<std::chrono::steady_clock::time_point> lastFrameTime_;

    std::shared_ptr<HardwareAccel> hwAccel_;  // 硬件加速器
    HWAccelType hwAccelType_ = HWAccelType::AUTO;
    int deviceIndex_ = 0;
    AVPixelFormat softPixelFormat_ = AV_PIX_FMT_YUV420P;
    bool requireFrameInMemory_ = false;

    // 复用的转换上下文和帧
    struct SwsContext *swsCtx_ = nullptr;
    Frame memoryFrame_;
    Frame swsFrame_;
};