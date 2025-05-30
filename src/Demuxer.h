#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

#include "EventDispatcher.h"
#include "PacketQueue.h"

class Demuxer {
public:
    Demuxer(std::shared_ptr<EventDispatcher> eventDispatcher);
    virtual ~Demuxer();

    bool open(const std::string &url, bool isRealTime);
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

    // 开始录像，暂时将输出文件保存为.mp4格式
    bool startRecording(const std::string &outputPath);
    // 停止录像
    bool stopRecording();
    // 是否正在录像
    bool isRecording() const;

protected:
    // 解复用线程
    void demuxLoop();
    // 录像线程
    void recordingLoop();

private:
    void start();
    void stop();

    // 初始化录像队列
    void initRecordQueue();
    // 销毁录像队列
    void destroyRecordQueue();
    // 获得录像队列
    std::shared_ptr<PacketQueue> recordPacketQueue(AVMediaType mediaType) const;

private:
    std::mutex mutex_;
    AVFormatContext *formatContext_ = nullptr;

    std::shared_ptr<PacketQueue> videoPacketQueue_;
    std::shared_ptr<PacketQueue> audioPacketQueue_;

    int videoStreamIndex_ = -1;  // 视频流索引
    int audioStreamIndex_ = -1;  // 音频流索引

    std::thread thread_;
    std::atomic<bool> isRunning_ = false;
    std::atomic<bool> isPaused_ = false;

    // 录制相关
    AVFormatContext *recordFormatCtx_ = nullptr;
    std::string recordFilePath_;
    std::atomic_bool isRecording_ = false;
    std::thread recordThread_;
    std::mutex recordMutex_;
    std::condition_variable recordCv_;
    std::atomic_bool recordStopFlag_ = false;

    // 录像队列相关
    std::shared_ptr<PacketQueue> videoRecordPacketQueue_;
    std::shared_ptr<PacketQueue> audioRecordPacketQueue_;

    // 事件分发器
    std::shared_ptr<EventDispatcher> eventDispatcher_;

    // 当前正在播放的路径
    std::string url_;
    // 是否是实时流
    bool isRealTime_ = false;
    // 是否需要关闭
    bool needClose_ = false;
};