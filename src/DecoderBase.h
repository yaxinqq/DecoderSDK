#pragma once
#include <atomic>
#include <thread>

#include "Clock.h"
#include "Demuxer.h"
#include "FrameQueue.h"
#include "SyncController.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

class DecoderBase {
public:
    DecoderBase(std::shared_ptr<Demuxer> demuxer, std::shared_ptr<SyncController> syncController);
    virtual ~DecoderBase();

    virtual bool open();
    virtual void start();
    virtual void stop();
    virtual void close();
    virtual void setSeekPos(double pos);

    FrameQueue& frameQueue();

    bool setSpeed(double speed);

    virtual AVMediaType type() const = 0;

protected:
    virtual void decodeLoop() = 0;

    // 根据情况，是否设置解码器的硬件解码
    virtual bool setHardwareDecode() { return false; };

    // 计算AVFrame对应的pts(单位 s)
    double calculatePts(AVFrame *frame) const;

protected:
    AVCodecContext *codecCtx_ = nullptr;
    
    int streamIndex_ = -1;
    AVStream *stream_ = nullptr; 
    
    std::shared_ptr<Demuxer> demuxer_;
    FrameQueue frameQueue_;

    std::thread thread_;
    std::atomic_bool isRunning_;

    std::atomic<double> speed_;
    std::atomic<double> seekPos_;

    std::condition_variable sleepCond_;
    std::mutex sleepMutex_;

    std::shared_ptr<SyncController> syncController_;  // 同步控制器
    bool frameRateControlEnabled_;  // 是否启用帧率控制
};