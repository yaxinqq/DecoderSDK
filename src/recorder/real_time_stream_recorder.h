#ifndef DECODER_SDK_INTERNAL_REAL_TIME_STREAM_RECORDER_H
#define DECODER_SDK_INTERNAL_REAL_TIME_STREAM_RECORDER_H
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

extern "C" {
#include <libavformat/avformat.h>
}

#include "base/base_define.h"
#include "base/packet.h"
#include "base/packet_queue.h"
#include "event_system/event_dispatcher.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

/**
 * @brief 支持的容器格式枚举
 */
enum class ContainerFormat {
    MP4,        // MP4容器
    AVI,        // AVI容器
    MKV,        // Matroska容器
    MOV,        // QuickTime容器
    FLV,        // Flash Video容器
    TS,         // MPEG-TS容器
    WEBM,       // WebM容器
    OGV,        // Ogg Video容器
    UNKNOWN     // 未知格式
};

/**
 * @brief 容器格式信息结构
 */
struct ContainerFormatInfo {
    ContainerFormat format;
    std::string formatName;     // FFmpeg格式名称
    std::string extension;      // 文件扩展名
    std::string description;    // 格式描述
    bool supportVideo;          // 是否支持视频
    bool supportAudio;          // 是否支持音频
    std::vector<std::string> supportedVideoCodecs;  // 支持的视频编解码器
    std::vector<std::string> supportedAudioCodecs;  // 支持的音频编解码器
};

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

    /**
     * @brief 获取支持的容器格式列表
     * @return 支持的容器格式信息列表
     */
    static std::vector<ContainerFormatInfo> getSupportedFormats();

    /**
     * @brief 检测文件路径对应的容器格式
     * @param filePath 文件路径
     * @return 检测到的容器格式
     */
    static ContainerFormat detectContainerFormat(const std::string &filePath);

    /**
     * @brief 验证容器格式是否支持指定的编解码器
     * @param format 容器格式
     * @param inputFormatCtx 输入格式上下文
     * @param errorMsg 错误信息输出
     * @return 是否支持
     */
    static bool validateFormatCompatibility(ContainerFormat format, 
                                          AVFormatContext *inputFormatCtx, 
                                          std::string &errorMsg);

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

    /**
     * @brief 获取容器格式信息映射表
     * @return 格式信息映射表
     */
    static const std::unordered_map<ContainerFormat, ContainerFormatInfo>& getFormatInfoMap();

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

    // 当前使用的容器格式
    ContainerFormat currentFormat_ = ContainerFormat::UNKNOWN;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_REAL_TIME_STREAM_RECORDER_H