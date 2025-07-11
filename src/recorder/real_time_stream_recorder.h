#ifndef DECODER_SDK_INTERNAL_REAL_TIME_STREAM_RECORDER_H
#define DECODER_SDK_INTERNAL_REAL_TIME_STREAM_RECORDER_H
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

#include "base/base_define.h"
#include "base/packet.h"
#include "base/packet_queue.h"
#include "event_system/event_dispatcher.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class RealTimeStreamRecorder {
public:
    /**
     * @brief 构造函数
     * @param eventDispatcher 事件分发器
     */
    explicit RealTimeStreamRecorder(std::shared_ptr<EventDispatcher> eventDispatcher);
    /**
     * @brief 析构函数
     */
    ~RealTimeStreamRecorder();

    // 禁用拷贝构造和拷贝赋值
    RealTimeStreamRecorder(const RealTimeStreamRecorder &) = delete;
    RealTimeStreamRecorder &operator=(const RealTimeStreamRecorder &) = delete;

    /**
     * @brief 开始录制
     * @param outputPath 输出文件路径
     * @param inputFormatCtx 输入格式上下文
     * @return 是否成功开始录制
     */
    bool startRecording(const std::string &outputPath, AVFormatContext *inputFormatCtx);

    /**
     * @brief 停止录制
     * @return 是否成功停止录制
     */
    bool stopRecording();

    /**
     * @brief 是否正在录制
     * @return 是否正在录制
     */
    bool isRecording() const;

    /**
     * @brief 写入数据包
     * @param packet 数据包
     * @param mediaType 媒体类型
     * @return 是否成功写入
     */
    bool writePacket(const Packet &packet, AVMediaType mediaType);

    /**
     * @brief 获取录制文件路径
     * @return 录制文件路径
     */
    std::string getRecordingPath() const;

private:
    /**
     * @brief 录制线程主循环
     */
    void recordingLoop();

    /**
     * @brief 初始化输出格式上下文
     * @param outputPath 输出文件路径
     * @param inputFormatCtx 输入格式上下文
     * @return 是否成功初始化
     */
    bool initOutputContext(const std::string &outputPath, AVFormatContext *inputFormatCtx);

    /**
     * @brief 清理资源
     */
    void cleanup();

    /**
     * @brief 创建流映射
     * @param inputFormatCtx 输入格式上下文
     * @return 是否成功创建
     */
    bool createStreamMapping(AVFormatContext *inputFormatCtx);

    /**
     * @brief 处理数据包写入
     * @param packet 数据包
     * @param mediaType 媒体类型
     * @return 是否成功写入
     */
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
    AVFormatContext *inputFormatCtx_ = nullptr; 
    std::string outputPath_;

    // 数据包队列 - 使用PacketQueue替代简单queue
    std::shared_ptr<PacketQueue> videoPacketQueue_;
    std::shared_ptr<PacketQueue> audioPacketQueue_;

    // 流映射表
    int *streamMapping_ = nullptr;
    int streamCount_ = 0;

    // 关键帧标志
    std::atomic<bool> hasKeyFrame_{false};

    // 流初始pts和dts映射表
    std::unordered_map<AVMediaType, int64_t> firstPtsMap_;
    std::unordered_map<AVMediaType, int64_t> firstDtsMap_;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_REAL_TIME_STREAM_RECORDER_H