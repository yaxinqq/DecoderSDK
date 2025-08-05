#ifndef DECODER_SDK_INTERNAL_DECODER_BASE_H
#define DECODER_SDK_INTERNAL_DECODER_BASE_H
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "base/base_define.h"
#include "base/frame_queue.h"
#include "base/packet_queue.h"
#include "include/decodersdk/common_define.h"
#include "stream_sync/clock.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class EventDispatcher;
class Demuxer;
class StreamSyncManager;

class DecoderBase {
public:
    /**
     * @brief 构造函数
     * @param demuxer 解复用器
     * @param StreamSyncManager 同步控制器
     * @param eventDispatcher 事件分发器
     */
    DecoderBase(std::shared_ptr<Demuxer> demuxer,
                std::shared_ptr<StreamSyncManager> StreamSyncManager,
                std::shared_ptr<EventDispatcher> eventDispatcher);
    /**
     * @brief 析构函数
     */
    virtual ~DecoderBase();

    /**
     * @brief 打开解码器
     * @return true 成功，false 失败
     */
    virtual bool open();
    /**
     * @brief 启动解码器（启动线程）
     */
    virtual void start();
    /**
     * @brief 停止解码器（停止线程）
     */
    virtual void stop();
    /**
     * @brief 暂停解码器（不停止线程，仅设置标志位）
     */
    virtual void pause();
    /**
     * @brief 恢复解码器（不重新启动线程，仅设置标志位）
     */
    virtual void resume();
    /**
     * @brief 关闭解码器
     */
    virtual void close();

    /**
     * @brief 获取帧队列
     * @return FrameQueue& 帧队列
     */
    std::shared_ptr<FrameQueue> frameQueue();

    /**
     * @brief 跳转到指定时间戳
     * @param pos 时间戳（单位 s）
     */
    virtual void setSeekPos(double pos);
    /**
     * @brief 获取当前跳转到的时间戳
     * @return double 时间戳（单位 s）
     */
    double seekPos() const;

    /**
     * @brief 设置倍速
     * @param speed 倍速
     * @return true 成功，false 失败
     */
    bool setSpeed(double speed);
    /**
     * @brief 获取倍速
     * @return double 倍速
     */
    double speed() const;

    /**
     * @brief 设置最大帧队列大小
     * @param size 最大帧队列大小
     */
    void setMaxFrameQueueSize(uint32_t size);
    /**
     * @brief 获取最大帧队列大小
     * @return uint32_t 最大帧队列大小
     */
    uint32_t maxFrameQueueSize() const;

    /**
     * @brief 设置是否按帧率去推送
     * @param enable 是否按帧率去推送
     */
    void setFrameRateControl(bool enable);
    /**
     * @brief 获取是否按帧率去推送
     * @return true 按帧率去推送，false 不按帧率去推送
     */
    bool isFrameRateControlEnabled() const;

    /**
     * @brief 设置解码最大连续错误容忍次数
     * @param maxErrors 最大连续错误容忍次数
     */
    void setMaxConsecutiveErrors(uint16_t maxErrors);
    /**
     * @brief 获取解码最大连续错误容忍次数
     * @return uint16_t 最大连续错误容忍次数
     */
    uint16_t maxConsecutiveErrors() const;

    /**
     * @brief 设置解码错误后，恢复的时间间隔
     * @param interval 时间间隔（ms）
     */
    void setRecoveryInterval(uint16_t interval);
    /**
     * @brief 获取解码错误后，恢复的时间间隔
     * @return uint16_t 时间间隔（ms）
     */
    uint16_t recoveryInterval() const;

    /**
     * @brief 获取解码器统计信息
     * @return const decoder_sdk::DecoderStatistics& 解码器统计信息
     */
    const decoder_sdk::DecoderStatistics &statistics() const;
    /**
     * @brief 重置解码器统计信息
     */
    void resetStatistics();
    /**
     * @brief 更新解码器统计信息中的总解码时间
     */
    void updateTotalDecodeTime();

    /**
     * @brief 获取解码器的媒体类型
     * @return AVMediaType 媒体类型
     */
    virtual AVMediaType type() const;

    /**
     * @brief 设置预缓冲等待状态
     * @param waiting 是否等待
     */
    void setWaitingForPreBuffer(bool waiting);

    /**
     * @brief 检查是否在等待预缓冲
     * @return true 正在等待，false 未等待
     */
    bool isWaitingForPreBuffer() const;

protected:
    /**
     * @brief 解码循环
     */
    virtual void decodeLoop() = 0;

    /**
     * @brief 根据情况，是否设置解码器的硬件解码
     * @return true 成功，false 失败
     */
    virtual bool setupHardwareDecode();

    /**
     * @brief 计算AVFrame对应的pts(单位 s)
     * @param frame 待计算的AVFrame
     * @return double pts（单位 s）
     */
    double calculatePts(const Frame &frame) const;

    // 公共的错误处理方法
    /**
     * @brief 处理第一个解码成功的帧
     * @param decoderName 解码器名称
     * @param mediaType 媒体类型
     * @param description 描述信息
     * @return true 成功，false 失败
     */
    bool handleFirstFrame(const std::string &decoderName, MediaType mediaType,
                          const std::string &description = "First frame ready!");
    /**
     * @brief 处理解码错误
     * @param decoderName 解码器名称
     * @param mediaType 媒体类型
     * @param errorCode 错误码
     * @param description 描述信息
     * @return true 成功，false 失败
     */
    bool handleDecodeError(const std::string &decoderName, MediaType mediaType, int errorCode,
                           const std::string &description = "Decoder error!");
    /**
     * @brief 处理解码恢复
     * @param decoderName 解码器名称
     * @param mediaType 媒体类型
     * @param description 描述信息
     * @return true 成功，false 失败
     */
    bool handleDecodeRecovery(const std::string &decoderName, MediaType mediaType,
                              const std::string &description = "Decoder Recovery!");

    /**
     * @brief 计算帧显示时间（单位 ms）
     * @param pts 帧的pts
     * @param duration 帧的持续时间
     * @param lastFrameTime 上一帧的时间点
     * @return double 帧显示时间（单位 ms）
     */
    double calculateFrameDisplayTime(
        double pts, double duration,
        std::optional<std::chrono::steady_clock::time_point> &lastFrameTime) const;

    /**
     * @brief 检查并更新序列号
     * @param currentSerial 当前序列号
     * @param packetQueue 数据包队列
     * @return true 序列号更新成功，false 序列号未更新
     */
    bool checkAndUpdateSerial(int &currentSerial, PacketQueue *packetQueue);

    /**
     * @brief 检查是否应该继续解码
     * @return true 应该继续解码，false 不应该继续解码
     */
    bool shouldContinueDecoding() const;

protected:
    AVCodecContext *codecCtx_ = nullptr; // 解码器上下文

    mutable std::mutex mutex_; // 同步原语
    bool isOpened_ = false;    // 解码器是否已开启
    bool isStarted_ = false;   // 解码器是否开始工作

    int streamIndex_ = -1;       // 流索引
    AVStream *stream_ = nullptr; // 流信息

    std::shared_ptr<Demuxer> demuxer_;       // 解复用器
    std::shared_ptr<FrameQueue> frameQueue_; // 帧队列

    std::thread thread_;                // 解码线程
    std::atomic_bool isPaused_ = false; // 解码线程暂停运行状态
    std::mutex pauseMutex_;             // 同步原语
    std::condition_variable pauseCv_;   // 用于暂停的条件变量

    std::atomic_bool requestInterruption_ = false; // 是否请求中断解码线程

    // 解码时间戳
    std::optional<std::chrono::steady_clock::time_point> lastFrameTime_;

    // 配置
    // 速度 此处存为整型，单位为 1000 倍
    std::atomic_uint16_t speed_ = 1000;
    // 跳转时间戳 ms，此处存为整型
    std::atomic_int64_t seekPosMs_ = -1;
    // demuxer是否正在seeking
    std::atomic_bool demuxerSeeking_ = false;
    // 是否按帧率去推送
    std::atomic_bool enableFrameControl_{true};
    // 解码最大连续错误容忍次数
    std::atomic_uint16_t maxConsecutiveErrors_{5};
    // 解码错误后，恢复的时间间隔（ms）
    std::atomic_uint16_t recoveryInterval_{3};

    // 同步控制器
    std::shared_ptr<StreamSyncManager> syncController_;

    // 统计信息
    mutable decoder_sdk::DecoderStatistics statistics_;

    // 事件分发器
    std::shared_ptr<EventDispatcher> eventDispatcher_;

    // 预缓冲状态
    std::atomic<bool> waitingForPreBuffer_{false};

private:
    /**
     * @brief 打开解码器，内部调用，不加锁
     */
    bool openInternal();
    /**
     * @brief 关闭解码器，内部调用，不加锁
     */
    void closeInternal();

    /**
     * @brief 启动解码器，内部调用，不加锁
     */
    void startInternal();
    /**
     * @brief 停止解码器，内部调用，不加锁
     */
    void stopInternal();

    /**
     * @brief 暂停解码器，内部调用，不加锁
     */
    void pauseInternal();
    /**
     * @brief 恢复解码器，内部调用，不加锁
     */
    void resumeInternal();
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_DECODER_BASE_H