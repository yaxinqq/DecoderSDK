#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

#include "EventDispatcher.h"
#include "Packet.h"
#include "PacketQueue.h"

class MediaRecorder {
public:
    explicit MediaRecorder(std::shared_ptr<EventDispatcher> eventDispatcher);
    ~MediaRecorder();

    // 禁用拷贝构造和拷贝赋值
    MediaRecorder(const MediaRecorder &) = delete;
    MediaRecorder &operator=(const MediaRecorder &) = delete;

    // 开始录制
    bool startRecording(const std::string &outputPath,
                        AVFormatContext *inputFormatCtx);

    // 停止录制
    bool stopRecording();

    // 是否正在录制
    bool isRecording() const;

    // 写入数据包
    bool writePacket(const Packet &packet, AVMediaType mediaType);

    // 获取录制文件路径
    std::string getRecordingPath() const;

private:
    // 录制线程主循环
    void recordingLoop();

    // 初始化输出格式上下文
    bool initOutputContext(const std::string &outputPath,
                           AVFormatContext *inputFormatCtx);

    // 清理资源
    void cleanup();

    // 创建流映射
    bool createStreamMapping(AVFormatContext *inputFormatCtx);

    // 处理数据包写入
    bool processPacket(const Packet &packet, AVMediaType mediaType);

private:
    // 事件分发器
    std::shared_ptr<EventDispatcher> eventDispatcher_;

    // 录制状态
    std::atomic<bool> isRecording_{false};
    std::atomic<bool> shouldStop_{false};

    // 录制线程
    std::thread recordingThread_;

    // 同步原语
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // FFmpeg相关
    AVFormatContext *outputFormatCtx_ = nullptr;
    std::string outputPath_;

    // 数据包队列 - 使用PacketQueue替代简单queue
    std::shared_ptr<PacketQueue> videoPacketQueue_;
    std::shared_ptr<PacketQueue> audioPacketQueue_;

    // 流映射表
    int *streamMapping_ = nullptr;
    int streamCount_ = 0;

    // 关键帧标志
    std::atomic<bool> hasKeyFrame_{false};
};