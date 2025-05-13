#pragma once
#include <atomic>
#include <thread>

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

    virtual AVMediaType type() const = 0;

protected:
    virtual void decodeLoop() = 0;

protected:
    AVCodecContext *codecCtx_ = nullptr;
    
    int streamIndex_ = -1;
    AVStream *stream_ = nullptr; 
    
    std::shared_ptr<Demuxer> demuxer_;
    FrameQueue frameQueue_;

    std::thread thread_;
    std::atomic_bool isRunning_;
};