#ifndef DECODER_SDK_INTERNAL_REAL_TIME_STREAM_RECORDER_H
#define DECODER_SDK_INTERNAL_REAL_TIME_STREAM_RECORDER_H
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
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
    MP4,    // MP4容器
    AVI,    // AVI容器
    MKV,    // Matroska容器
    MOV,    // QuickTime容器
    FLV,    // Flash Video容器
    TS,     // MPEG-TS容器
    WEBM,   // WebM容器
    OGV,    // Ogg Video容器
    UNKNOWN // 未知格式
};

/**
 * @brief 容器格式信息结构
 */
struct ContainerFormatInfo {
    ContainerFormat format;
    std::string formatName;                        // FFmpeg格式名称
    std::string extension;                         // 文件扩展名
    std::string description;                       // 格式描述
    bool supportVideo;                             // 是否支持视频
    bool supportAudio;                             // 是否支持音频
    std::vector<std::string> supportedVideoCodecs; // 支持的视频编解码器
    std::vector<std::string> supportedAudioCodecs; // 支持的音频编解码器
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
     * @param recordMediaType 需要录制的媒体类型
     * @return 是否成功开始录制
     */
    bool startRecording(const std::string &outputPath, AVFormatContext *inputFormatCtx,
                        MediaTypes recordMediaTypes);

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
     * @brief 设置预缓冲时长（毫秒）。
     *
     * 启用后，writePacket() 会将数据包缓存到内部环形缓冲区，
     * 直到调用 startRecording() 时才将缓冲区内容写入文件。
     * 时长为 0 表示禁用预缓冲（默认行为）。
     *
     * @param durationMs 预缓冲最大时长（毫秒）
     */
    void setPrebufferDuration(int64_t durationMs);

    /**
     * @brief 清除预缓冲区中的所有数据包。
     */
    void clearPrebuffer();

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
     * @param recordMediaType 需要录制的媒体类型
     * @param errorMsg 错误信息输出
     * @return 是否支持
     */
    static bool validateFormatCompatibility(ContainerFormat format, AVFormatContext *inputFormatCtx,
                                            MediaTypes recordMediaTypes, std::string &errorMsg);

private:
    /**
     * @brief 录制线程主循环
     */
    void recordingLoop();

    /**
     * @brief 初始化输出格式上下文
     * @param outputPath 输出文件路径
     * @param inputFormatCtx 输入格式上下文
     * @return 是否成功初始化，返回错误码，0 - 初始化成功
     */
    int initOutputContext(const std::string &outputPath, AVFormatContext *inputFormatCtx,
                          MediaTypes recordMediaTypes);

    /**
     * @brief 清理资源
     */
    void cleanup();

    /**
     * @brief 创建流映射
     * @param inputFormatCtx 输入格式上下文
     * @return 是否成功创建
     */
    bool createStreamMapping(AVFormatContext *inputFormatCtx, MediaTypes recordMediaTypes);

    /**
     * @brief 处理数据包写入
     * @param packet 数据包
     * @param mediaType 媒体类型
     * @param hasKeyFrame 是否是关键帧
     * @return 是否成功写入
     */
    bool processPacket(const Packet &packet, AVMediaType mediaType, bool &hasKeyFrame);

    /**
     * @brief 发送录制错误事件
     *
     * @param ret 错误码
     * @param errStr 错误信息
     * @return
     */
    void triggerErrorEvent(const std::string &outputPath, ContainerFormat format, int ret,
                           const std::string &errStr);

    /**
     * @brief 获取容器格式信息映射表
     * @return 格式信息映射表
     */
    static const std::unordered_map<ContainerFormat, ContainerFormatInfo> &getFormatInfoMap();

    /**
     * @brief 将预缓冲区的数据包刷新到录制文件。
     *
     * 在 startRecording() 初始化输出上下文之后调用，
     * 将缓冲区中的预事件数据写入文件。
     */
    void flushPrebufferToRecording();

    /**
     * @brief 初始化音频转码上下文
     *
     * 当输入音频编解码器不被输出容器支持时，创建解码器、编码器和重采样上下文，
     * 将输入音频转码为目标编解码器（如 AAC）。
     *
     * @param inStream 输入音频流
     * @param outStream 输出音频流
     * @return 是否成功初始化
     */
    bool initAudioTranscoder(AVStream *inStream, AVStream *outStream);

    /**
     * @brief 转码并写入音频数据包
     *
     * 解码输入音频帧，重采样为目标格式，编码后写入输出文件。
     *
     * @param packet 输入音频数据包
     * @param outStream 输出音频流
     * @return 是否成功写入
     */
    bool transcodeAndWriteAudioPacket(AVPacket *packet, AVStream *outStream);

    /**
     * @brief 刷新音频转码器
     *
     * 在录制结束时调用，刷新编码器缓冲区中剩余的数据。
     */
    void flushAudioTranscoder();

    /**
     * @brief 释放音频转码资源
     */
    void cleanupAudioTranscoder();

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

    // 流初始pts和dts映射表
    std::unordered_map<AVMediaType, int64_t> firstPtsMap_;
    std::unordered_map<AVMediaType, int64_t> firstDtsMap_;

    // 当前使用的容器格式
    ContainerFormat currentFormat_ = ContainerFormat::UNKNOWN;

    // 预缓冲环形缓冲区：存储 startRecording 之前收到的数据包
    struct TimestampedPacket {
        Packet packet;
        AVMediaType mediaType;
    };
    std::deque<TimestampedPacket> prebuffer_;
    int64_t prebufferDurationMs_ = 0;  // 0 = 预缓冲禁用
    int64_t prebufferTotalDurationUs_ = 0;
    std::mutex prebufferMutex_;  // 保护 prebuffer_ 的并发访问

    // 预缓冲录制器的键（非空表示该录制器由 startPrebufferedRecording 创建）。
    // 录制停止后，Demuxer 通过此键将录制器重新插入映射以便复用。
    std::string prebufferKey_;

    // 音频转码器状态（当输入音频编解码器不被输出容器支持时使用）
    struct AudioTranscoderState {
        AVCodecContext *decoderCtx = nullptr;   // 输入音频解码器上下文
        AVCodecContext *encoderCtx = nullptr;   // 目标音频编码器上下文
        SwrContext *swrCtx = nullptr;           // 重采样上下文
        AVAudioFifo *audioFifo = nullptr;        // 音频样本 FIFO 缓冲
        AVFrame *resampleFrame = nullptr;        // 重采样输出帧（复用缓冲）
        AVFrame *fifoFrame = nullptr;            // FIFO 读出帧（复用缓冲）
        int outStreamIndex = -1;                 // 输出音频流索引，用于收尾写入时定位正确的轨道
        int64_t pts = 0;                         // 编码器 PTS（以样本数累加）
        bool initialized = false;                // 是否已成功初始化
    };
    std::unique_ptr<AudioTranscoderState> audioTranscoder_;

public:
    /// 获取预缓冲键。
    const std::string &prebufferKey() const { return prebufferKey_; }
    /// 设置预缓冲键（由 Demuxer::startPrebufferedRecording 调用）。
    void setPrebufferKey(const std::string &key) { prebufferKey_ = key; }
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_REAL_TIME_STREAM_RECORDER_H