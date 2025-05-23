#pragma once
#include "Clock.h"
#include "DecoderBase.h"

#include <optional>

extern "C" {
#include "libswresample/swresample.h"
}

class AudioDecoder : public DecoderBase
{
public:
    AudioDecoder(std::shared_ptr<Demuxer> demuxer, std::shared_ptr<SyncController> syncController);
    virtual ~AudioDecoder();

    AVMediaType type() const override;

protected:
    virtual void decodeLoop() override;
    
private:
    // 初始化重采样上下文
    bool initResampleContext();

    // 重采样音频数据
    AVFrame *resampleFrame(AVFrame *frame);

    double calculateFrameDisplayTime(double pts, double duration);

private:
    SwrContext *swrCtx_ = nullptr;  // 用于音频重采样
    bool needResample_ = false;     // 是否需要重采样

    std::optional<std::chrono::steady_clock::time_point> lastFrameTime_;          // 上一帧的时间点
};