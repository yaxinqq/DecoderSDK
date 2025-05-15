#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "PacketQueue.h"

extern "C" {
#include <libavformat/avformat.h>
}

class Demuxer
{
public:
	Demuxer();
	virtual ~Demuxer();

    bool open(const std::string& url);
    void close();

    void start();
    void stop();

    bool pause();
    bool resume();

    bool seek(double position);
    
    AVFormatContext* formatContext() const;
    int streamIndex(AVMediaType mediaType) const;
    std::shared_ptr<PacketQueue> packetQueue(AVMediaType mediaType) const;

    // 检查是否有视频流
    bool hasVideo() const;
    
    // 检查是否有音频流
    bool hasAudio() const;

    // 是否暂停
    bool isPaused() const;

protected:
    void demuxLoop();

private:
    std::mutex mutex_;
    AVFormatContext* formatContext_ = nullptr;

    std::shared_ptr<PacketQueue> videoPacketQueue_;
    std::shared_ptr<PacketQueue> audioPacketQueue_;

    int videoStreamIndex_ = -1; // 视频流索引
    int audioStreamIndex_ = -1; // 音频流索引

    std::thread thread_;
    std::atomic<bool> isRunning_ = false;
    std::atomic<bool> isPaused_ = false;
};