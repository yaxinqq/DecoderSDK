#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

#include "EventDispatcher.h"
#include "MediaRecorder.h"
#include "PacketQueue.h"

class Demuxer {
public:
    explicit Demuxer(std::shared_ptr<EventDispatcher> eventDispatcher);
    virtual ~Demuxer();

    // 禁用拷贝构造和拷贝赋值
    Demuxer(const Demuxer &) = delete;
    Demuxer &operator=(const Demuxer &) = delete;

    bool open(const std::string &url, bool isRealTime, bool isReopen = false);
    bool close();

    bool pause();
    bool resume();

    bool seek(double position);

    AVFormatContext *formatContext() const;
    int streamIndex(AVMediaType mediaType) const;
    std::shared_ptr<PacketQueue> packetQueue(AVMediaType mediaType) const;

    // 检查是否有视频流
    bool hasVideo() const;

    // 检查是否有音频流
    bool hasAudio() const;

    // 是否暂停
    bool isPaused() const;

    // 是否是实时流
    bool isRealTime() const;

    // 当前正在播放的路径
    std::string url() const;

    // 录制相关 - 委托给MediaRecorder
    bool startRecording(const std::string &outputPath);
    bool stopRecording();
    bool isRecording() const;

protected:
    // 解复用线程
    void demuxLoop();

private:
    void start();
    void stop();

    void handleEndOfFile(AVPacket *pkt);
    void distributePacket(AVPacket *pkt);
    void waitForQueueEmpty();

private:
    // 同步原语
    std::mutex mutex_;
    std::condition_variable pauseCv_;

    // FFmpeg相关
    AVFormatContext *formatContext_ = nullptr;

    // 数据包队列
    std::shared_ptr<PacketQueue> videoPacketQueue_;
    std::shared_ptr<PacketQueue> audioPacketQueue_;

    // 流索引
    int videoStreamIndex_ = -1;
    int audioStreamIndex_ = -1;

    // 线程管理
    std::thread thread_;
    std::atomic<bool> isRunning_{false};
    std::atomic<bool> isPaused_{false};

    // 录制器
    std::unique_ptr<MediaRecorder> mediaRecorder_;

    // 事件分发器
    std::shared_ptr<EventDispatcher> eventDispatcher_;

    // 状态信息
    std::string url_;
    bool isRealTime_ = false;
    bool needClose_ = false;
    bool isReopen_ = false;
};