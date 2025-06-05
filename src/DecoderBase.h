#pragma once
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

#include "Clock.h"
#include "Demuxer.h"
#include "EventDispatcher.h"
#include "FrameQueue.h"
#include "SyncController.h"

// 性能指标结构
struct DecoderStatistics {
    // 解码的帧数
    std::atomic<uint64_t> framesDecoded{0};
    // 错误统计 AVERROR_EOF 和 AVERROR(EAGAIN) 不计算在内
    std::atomic<uint64_t> errorsCount{0};
    // 解码时间统计 ms
    std::atomic<uint64_t> totalDecodeTime{0};
    // 连续错误次数
    std::atomic<uint64_t> consecutiveErrors{0};
    // 开始时间
    std::chrono::steady_clock::time_point startTime;

    void reset()
    {
        framesDecoded.store(0);
        errorsCount.store(0);
        totalDecodeTime.store(0);
        consecutiveErrors.store(0);
        startTime = std::chrono::steady_clock::now();
    }

    double getFrameRate() const
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now() - startTime)
                           .count();
        return elapsed > 0 ? static_cast<double>(framesDecoded.load()) / elapsed
                           : 0.0;
    }
};

class DecoderBase {
public:
    DecoderBase(std::shared_ptr<Demuxer> demuxer,
                std::shared_ptr<SyncController> syncController,
                std::shared_ptr<EventDispatcher> eventDispatcher);
    virtual ~DecoderBase();

    virtual bool open();
    virtual void start();
    virtual void stop();
    virtual void close();

    FrameQueue &frameQueue();

    // 跳转到指定时间戳
    virtual void setSeekPos(double pos);
    double seekPos() const;

    // 倍速
    bool setSpeed(double speed);
    double speed() const;

    // 最大帧队列大小（建议在解码之前调用）
    void setMaxFrameQueueSize(uint32_t size);
    uint32_t maxFrameQueueSize() const;

    // 是否按帧率去推送
    void setFrameRateControl(bool enable);
    bool isFrameRateControlEnabled() const;

    // 解码最大连续错误容忍次数
    void setMaxConsecutiveErrors(uint16_t maxErrors);
    uint16_t maxConsecutiveErrors() const;

    // 解码错误后，恢复的时间间隔（ms）
    void setRecoveryInterval(uint16_t interval);
    uint16_t recoveryInterval() const;

    // 性能监控
    const DecoderStatistics &statistics() const;
    void resetStatistics();
    void updateTotalDecodeTime();

    // 媒体类型
    virtual AVMediaType type() const = 0;

protected:
    // 解码循环
    virtual void decodeLoop() = 0;

    // 根据情况，是否设置解码器的硬件解码
    virtual bool setHardwareDecode();

    // 计算AVFrame对应的pts(单位 s)
    double calculatePts(const Frame &frame) const;

    // 公共的错误处理方法
    bool handleFirstFrame(
        const std::string &decoderName, MediaType mediaType,
        const std::string &description = "First frame ready!");
    bool handleDecodeError(const std::string &decoderName, MediaType mediaType,
                           int errorCode,
                           const std::string &description = "Decoder error!");
    bool handleDecodeRecovery(
        const std::string &decoderName, MediaType mediaType,
        const std::string &description = "Decoder Recovery!");

    // 公共的时间计算方法
    double calculateFrameDisplayTime(
        double pts, double duration,
        std::optional<std::chrono::steady_clock::time_point> &lastFrameTime);

    // 公共的序列号检查
    bool checkAndUpdateSerial(int &currentSerial, PacketQueue *packetQueue);

    // 检查是否应该继续解码
    bool shouldContinueDecoding() const;

protected:
    AVCodecContext *codecCtx_ = nullptr;
    bool needClose_ = false;

    int streamIndex_ = -1;
    AVStream *stream_ = nullptr;

    std::shared_ptr<Demuxer> demuxer_;
    FrameQueue frameQueue_;

    std::thread thread_;
    std::atomic_bool isRunning_;

    // 使用mutex保护的配置变量
    mutable std::mutex configMutex_;
    // 速度
    double speed_ = 1.0;
    // 跳转时间戳
    double seekPos_ = 0.0;
    // 最大帧队列大小
    std::atomic_uint32_t maxFrameQueueSize_{3};
    // 是否按帧率去推送
    std::atomic_bool enableFrameControl_{true};
    // 解码最大连续错误容忍次数
    std::atomic_uint16_t maxConsecutiveErrors_{5};
    // 解码错误后，恢复的时间间隔（ms）
    std::atomic_uint16_t recoveryInterval_{3};

    std::condition_variable sleepCond_;
    std::mutex sleepMutex_;

    // 同步控制器
    std::shared_ptr<SyncController> syncController_;

    // 统计信息
    mutable DecoderStatistics statistics_;

    // 事件分发器
    std::shared_ptr<EventDispatcher> eventDispatcher_;
};