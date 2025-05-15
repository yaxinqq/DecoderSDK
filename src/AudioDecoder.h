#pragma once
#include "Clock.h"
#include "DecoderBase.h"

extern "C" {
#include "libswresample/swresample.h"
}

class AudioDecoder : public DecoderBase
{
public:
    AudioDecoder(std::shared_ptr<Demuxer> demuxer);
    virtual ~AudioDecoder();

    AVMediaType type() const override;
    
    // 获取音频时钟
    Clock* getClock() { return &clock_; }

protected:
    virtual void decodeLoop() override;
    // 计算包队列的最大包数量
    int calculateMaxPacketCount() const override; 
    
private:
    // 初始化重采样上下文
    bool initResampleContext();

    // 重采样音频数据
    AVFrame *resampleFrame(AVFrame *frame);

private:
    SwrContext *swrCtx_ = nullptr;  // 用于音频重采样
    bool needResample_ = false;     // 是否需要重采样
};