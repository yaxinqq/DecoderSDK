#ifndef DECODER_SDK_INTERNAL_DEMUXER_H
#define DECODER_SDK_INTERNAL_DEMUXER_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

#include "base/base_define.h"
#include "base/packet.h"
#include "event_system/event_dispatcher.h"
#include "recorder/real_time_stream_recorder.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class Demuxer {
public:
    /**
     * @brief 构造函数
     * @param eventDispatcher 事件分发器
     */
    explicit Demuxer(std::shared_ptr<EventDispatcher> eventDispatcher);
    /**
     * @brief 析构函数
     */
    virtual ~Demuxer();

    // 禁用拷贝构造和拷贝赋值
    Demuxer(const Demuxer &) = delete;
    Demuxer &operator=(const Demuxer &) = delete;

    /**
     * @brief 打开媒体文件
     * @param url 媒体文件路径
     * @param isRealTime 是否是实时流
     * @param decodeMediaType 解码的媒体类型
     * @param isReopen 是否重新打开
     * @return 是否成功打开
     */
    bool open(const std::string &url, bool isRealTime, Config::DecodeMediaType decodeMediaType,
              bool isReopen = false);
    /**
     * @brief 关闭媒体文件
     * @return 是否成功关闭
     */
    bool close();

    /**
     * @brief 暂停解复用
     * @return 是否成功暂停
     */
    bool pause();
    /**
     * @brief 恢复解复用
     * @return 是否成功恢复
     */
    bool resume();

    /**
     * @brief 定位到指定位置
     * @param position 定位时间点（单位：秒）
     * @return 是否成功定位
     */
    bool seek(double position);

    /**
     * @brief 格式上下文
     */
    AVFormatContext *formatContext() const;
    /**
     * @brief 流索引
     */
    int streamIndex(AVMediaType mediaType) const;
    /**
     * @brief 数据包队列
     */
    std::shared_ptr<PacketQueue> packetQueue(AVMediaType mediaType) const;

    /**
     * @brief 检查是否有视频流
     * @return 是否有视频流
     */
    bool hasVideo() const;

    /**
     * @brief 检查是否有音频流
     * @return 是否有音频流
     */
    bool hasAudio() const;

    /**
     * @brief 是否暂停
     * @return 是否暂停
     */
    bool isPaused() const;

    /**
     * @brief 是否是实时流
     * @return 是否是实时流
     */
    bool isRealTime() const;

    /**
     * @brief 当前正在播放的路径
     * @return 路径
     */
    std::string url() const;

    /**
     * @brief 开始录制
     * @param outputPath 输出文件路径
     * @return 是否成功开始录制
     */
    bool startRecording(const std::string &outputPath);
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
     * @brief 设置预缓冲配置
     * @param videoFrames 视频缓冲帧数
     * @param audioPackets 音频缓冲包数
     * @param requireBoth 是否需要同时缓冲视频和音频
     * @param onPreBufferReady 预缓冲就绪回调
     */
    void setPreBufferConfig(int videoFrames, int audioPackets, bool requireBoth,
                            std::function<void()> onPreBufferReady = nullptr);

    /**
     * @brief 检查预缓冲状态
     * @return 是否已达到预缓冲要求
     */
    bool isPreBufferReady() const;

    /**
     * @brief 获取预缓冲进度
     * @return 预缓冲进度
     */
    PreBufferProgress getPreBufferProgress() const;

    /**
     * @brief 清理预缓冲回调
     */
    void clearPreBufferCallback();

    /**
     * @brief 设置循环播放模式
     * @param mode 循环模式
     * @param maxLoops 最大循环次数（仅在kSingle模式下有效，-1表示无限循环）
     */
    void setLoopMode(LoopMode mode, int maxLoops = -1);

    /**
     * @brief 获取循环播放模式
     * @return 当前循环模式
     */
    LoopMode getLoopMode() const;

    /**
     * @brief 获取当前循环次数
     * @return 当前循环次数
     */
    int getCurrentLoopCount() const;

    /**
     * @brief 重置循环计数
     */
    void resetLoopCount();

protected:
    /**
     * @brief 解复用线程
     */
    void demuxLoop();

private:
    /**
     * @brief 启动解复用线程
     */
    void start();
    /**
     * @brief 停止解复用线程
     */
    void stop();

    /**
     * @brief 处理文件结束
     * @param pkt 数据包
     */
    void handleEndOfFile(AVPacket *pkt);
    /**
     * @brief 分发数据包
     * @param pkt 数据包
     */
    void distributePacket(AVPacket *pkt);
    /**
     * @brief 等待队列清空
     */
    void waitForQueueEmpty();

    /**
     * @brief 文件流解复用循环
     * @param pkt 数据包
     */
    void fileStreamDemuxLoop(AVPacket *pkt);

    /**
     * @brief 实时流解复用循环
     * @param pkt 数据包
     */
    void realTimeStreamDemuxLoop(AVPacket *pkt);

    /**
     * @brief 处理暂停状态（文件流）
     * @return 是否应该继续循环
     */
    bool handleFileStreamPause();

    /**
     * @brief 读取并处理数据包
     * @param pkt 数据包
     * @param occuredErrorTime 出错时间
     * @param readFirstPacket 是否已读取首包引用
     * @param isEof 是否已经处理过eof
     * @return 读取结果：0=成功，1=EOF，-1=错误需要继续，-2=严重错误需要退出
     */
    int readAndProcessPacket(
        AVPacket *pkt,
        std::optional<std::chrono::high_resolution_clock::time_point> &occuredErrorTime,
        bool &readFirstPacket, bool isEof = false);

    /**
     * @brief 检查预缓冲状态
     */
    void checkPreBufferStatus();

    /**
     * @brief 处理循环播放
     * @return true 成功开始新的循环; false 不需要循环或循环失败
     */
    bool handleLoopPlayback();

    /**
     * @brief 处理读取错误
     * @param occuredErrorTime 出错时间s
     * @return 处理结果：-1=需要继续，-2=严重错误
     */
    int handleReadError(
        std::optional<std::chrono::high_resolution_clock::time_point> &occuredErrorTime);

    /**
     * @brief 处理seek请求
     * @return true 如果处理了seek请求; false 如果没有pending的seek请求
     */
    bool handleSeekRequest();

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
    std::atomic<bool> isSeeking_{false};

    // seek位置原子变量
    std::atomic_int seekMsPos_ = -1;

    // 录制器
    std::unique_ptr<RealTimeStreamRecorder> realTimeStreamRecorder_;

    // 事件分发器
    std::shared_ptr<EventDispatcher> eventDispatcher_;

    // 状态信息
    std::string url_;
    std::atomic_bool isRealTime_ = false;
    bool needClose_ = false;
    bool isReopen_ = false;

    // 预缓冲配置
    int preBufferVideoFrames_ = 0;
    int preBufferAudioPackets_ = 0;
    bool requireBothStreams_ = false;
    std::atomic<bool> preBufferEnabled_{false};
    std::atomic<bool> preBufferReady_{false};
    std::function<void()> preBufferReadyCallback_;

    // 循环播放相关成员变量
    std::atomic<LoopMode> loopMode_{LoopMode::kNone};
    std::atomic<int> maxLoops_{-1};
    std::atomic<int> currentLoopCount_{0};
    std::mutex loopMutex_;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_DEMUXER_H
