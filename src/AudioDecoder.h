#pragma once
#include "Clock.h"
#include "DecoderBase.h"

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
    
private:
    Clock clock_; // 用于音频同步
};