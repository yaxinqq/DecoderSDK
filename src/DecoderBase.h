#pragma once
#include <atomic>
#include <thread>

#include "Clock.h"
#include "Demuxer.h"
#include "FrameQueue.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

class DecoderBase {
public:
    DecoderBase(std::shared_ptr<Demuxer> demuxer);
    virtual ~DecoderBase();

    virtual bool open();
    virtual void start();
    virtual void stop();
    virtual void close();

    FrameQueue& frameQueue();

    bool setSpeed(double speed);

    virtual AVMediaType type() const = 0;

protected:
    virtual void decodeLoop() = 0;

    // 根据情况，是否设置解码器的硬件解码
    virtual bool setHardwareDecode() { return false; };
    // 计算包队列的最大包数量
    virtual int calculateMaxPacketCount() const; 

protected:
    AVCodecContext *codecCtx_ = nullptr;
    
    int streamIndex_ = -1;
    AVStream *stream_ = nullptr; 
    
    std::shared_ptr<Demuxer> demuxer_;
    FrameQueue frameQueue_;

    std::thread thread_;
    std::atomic_bool isRunning_;

    std::atomic<double> speed_;

    std::condition_variable sleepCond_;
    std::mutex sleepMutex_;

    Clock clock_; // 用于同步
};