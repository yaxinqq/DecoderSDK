#include "decoder_base.h"

#include <algorithm>
#include <thread>

#include "demuxer/demuxer.h"
#include "event_system/event_dispatcher.h"
#include "logger/Logger.h"
#include "stream_sync/stream_sync_manager.h"
#include "utils/common_utils.h"

namespace {
constexpr int kFrameQueueDefaultSize = 3;

decoder_sdk::MediaType transAVMediaType(AVMediaType type)
{
    switch (type) {
        case AVMEDIA_TYPE_VIDEO:
            return decoder_sdk::MediaType::kMediaTypeVideo;
        case AVMEDIA_TYPE_AUDIO:
            return decoder_sdk::MediaType::kMediaTypeAudio;
        default:
            return decoder_sdk::MediaType::kMediaTypeUnknown;
    }
}
} // namespace

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

DecoderBase::DecoderBase(std::shared_ptr<Demuxer> demuxer,
                         std::shared_ptr<StreamSyncManager> StreamSyncManager,
                         std::shared_ptr<EventDispatcher> eventDispatcher)
    : demuxer_(demuxer),
      syncController_(StreamSyncManager),
      eventDispatcher_(eventDispatcher),
      frameQueue_(new FrameQueue(kFrameQueueDefaultSize, false)),
      isRunning_(false),
      lastFrameTime_(std::nullopt)
{
    statistics_.reset();
}

DecoderBase::~DecoderBase()
{
    close();
}

bool DecoderBase::open()
{
    const auto sendFailedEvent = [this]() {
        auto event = std::make_shared<DecoderEventArgs>(codecCtx_ ? codecCtx_->codec->name : "",
                                                        streamIndex_, transAVMediaType(type()),
                                                        false, "Decoder", "Decode Created Failed");
        eventDispatcher_->triggerEvent(EventType::kCreateDecoderFailed, event);
    };

    auto *const formatContext = demuxer_->formatContext();
    if (!formatContext) {
        sendFailedEvent();
        return false;
    }

    streamIndex_ = demuxer_->streamIndex(type());
    if (streamIndex_ < 0) {
        sendFailedEvent();
        return false;
    }

    stream_ = formatContext->streams[streamIndex_];

    const AVCodec *codec = avcodec_find_decoder(stream_->codecpar->codec_id);
    if (!codec) {
        sendFailedEvent();
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        sendFailedEvent();
        return false;
    }

    if (avcodec_parameters_to_context(codecCtx_, stream_->codecpar) < 0) {
        sendFailedEvent();
        return false;
    }

    // 尝试设置硬解
    const bool useHw = setupHardwareDecode();

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        sendFailedEvent();
        return false;
    }

    // 发送解码器创建成功的事件
    auto event = std::make_shared<DecoderEventArgs>(codecCtx_->codec->name, streamIndex_,
                                                    transAVMediaType(type()), useHw, "Decoder",
                                                    "Decode Created Success");
    eventDispatcher_->triggerEvent(EventType::kCreateDecoderSuccess, event);

    needClose_ = true;
    statistics_.reset();
    return true;
}

void DecoderBase::start()
{
    if (isRunning_)
        return;

    // 获取对应的包队列
    auto packetQueue = demuxer_->packetQueue(type());
    if (!packetQueue)
        return;

    // 设置帧队列的序列号与包队列一致
    frameQueue_->setSerial(packetQueue->serial());
    // 设置帧队列的中止状态与包队列一致
    frameQueue_->setAbortStatus(packetQueue->isAborted());

    // 清空seek节点
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        seekPos_ = 0.0;
    }

    isRunning_ = true;
    thread_ = std::thread(&DecoderBase::decodeLoop, this);

    // 发送解码已开始的事件
    auto event = std::make_shared<DecoderEventArgs>(
        codecCtx_->codec->name, streamIndex_, transAVMediaType(type()),
        codecCtx_->hw_device_ctx != nullptr, "Decoder", "Decode Started");
    eventDispatcher_->triggerEvent(EventType::kDecodeStarted, event);
}

void DecoderBase::stop()
{
    if (!isRunning_)
        return;

    isRunning_ = false;
    frameQueue_->setAbortStatus(true);

    sleepCond_.notify_all();

    if (thread_.joinable())
        thread_.join();

    // 发送解码已停止的事件
    auto event = std::make_shared<DecoderEventArgs>(
        codecCtx_->codec->name, streamIndex_, transAVMediaType(type()),
        codecCtx_->hw_device_ctx != nullptr, "Decoder", "Decode Stopped");
    eventDispatcher_->triggerEvent(EventType::kDecodeStopped, event);
}

void DecoderBase::close()
{
    stop();
    if (!needClose_)
        return;

    const auto codecName = codecCtx_ ? codecCtx_->codec->name : "";
    const bool useHw = codecCtx_ ? codecCtx_->hw_device_ctx != nullptr : false;

    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
    }

    needClose_ = false;

    // 发送解码已销毁的事件
    auto event = std::make_shared<DecoderEventArgs>(
        codecName, streamIndex_, transAVMediaType(type()), useHw, "Decoder", "Decode Destroyed");
    eventDispatcher_->triggerEvent(EventType::kDestoryDecoder, event);
}

std::shared_ptr<FrameQueue> DecoderBase::frameQueue()
{
    return frameQueue_;
}

void DecoderBase::setSeekPos(double pos)
{
    std::lock_guard<std::mutex> lock(configMutex_);
    seekPos_ = pos;
}

double DecoderBase::seekPos() const
{
    std::lock_guard<std::mutex> lock(configMutex_);
    return seekPos_;
}

bool DecoderBase::setSpeed(double speed)
{
    if (speed <= 0.0f) {
        return false;
    }

    std::lock_guard<std::mutex> lock(configMutex_);
    if (utils::equal(speed_, speed)) {
        return false;
    }

    speed_ = speed;
    syncController_->setSpeed(speed);
    return true;
}

double DecoderBase::speed() const
{
    std::lock_guard<std::mutex> lock(configMutex_);
    return speed_;
}

void DecoderBase::setMaxFrameQueueSize(uint32_t size)
{
    if (maxFrameQueueSize_.load() == size) {
        return;
    }

    maxFrameQueueSize_.store(size);
    frameQueue_->setMaxCount(size);
}

uint32_t DecoderBase::maxFrameQueueSize() const
{
    return maxFrameQueueSize_.load();
}

void DecoderBase::setFrameRateControl(bool enable)
{
    if (enableFrameControl_.load() == enable) {
        return;
    }

    enableFrameControl_.store(enable);
}

bool DecoderBase::isFrameRateControlEnabled() const
{
    return enableFrameControl_.load();
}

void DecoderBase::setMaxConsecutiveErrors(uint16_t maxErrors)
{
    if (maxConsecutiveErrors_.load() == maxErrors) {
        return;
    }

    maxConsecutiveErrors_.store(maxErrors);
}

uint16_t DecoderBase::maxConsecutiveErrors() const
{
    return maxConsecutiveErrors_.load();
}

void DecoderBase::setRecoveryInterval(uint16_t interval)
{
    if (recoveryInterval_.load() == interval) {
        return;
    }

    recoveryInterval_.store(interval);
}

uint16_t DecoderBase::recoveryInterval() const
{
    return recoveryInterval_.load();
}

const DecoderStatistics &DecoderBase::statistics() const
{
    return statistics_;
}

void DecoderBase::resetStatistics()
{
    statistics_.reset();
}

void DecoderBase::updateTotalDecodeTime()
{
    statistics_.totalDecodeTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - statistics_.startTime)
                                      .count();
}

void DecoderBase::setWaitingForPreBuffer(bool waiting)
{
    waitingForPreBuffer_.store(waiting);
    if (!waiting) {
        // 预缓冲完成，唤醒解码线程
        LOG_INFO("Video decoder: pre-buffer completed, resuming decode.");
    }
}

bool DecoderBase::isWaitingForPreBuffer() const
{
    return waitingForPreBuffer_.load();
}

bool DecoderBase::setupHardwareDecode()
{
    return false;
}

double DecoderBase::calculatePts(const Frame &frame) const
{
    if (!frame.isValid())
        return -1.0;

    const int64_t pts =
        (frame.avPts() != AV_NOPTS_VALUE) ? frame.avPts() : frame.bestEffortTimestamp();
    const double time = pts * av_q2d(stream_->time_base);
    return time;
}

bool DecoderBase::handleFirstFrame(const std::string &decoderName, MediaType mediaType,
                                   const std::string &description)
{
    auto event = std::make_shared<DecoderEventArgs>(decoderName, streamIndex_, mediaType,
                                                    codecCtx_->hw_device_ctx != nullptr,
                                                    decoderName, description);
    eventDispatcher_->triggerEvent(EventType::kDecodeFirstFrame, event);

    return true;
}

bool DecoderBase::handleDecodeError(const std::string &decoderName, MediaType mediaType,
                                    int errorCode, const std::string &description)
{
    if (errorCode == AVERROR_EOF || errorCode == AVERROR(EAGAIN))
        return false;

    statistics_.errorsCount.fetch_add(1);
    LOG_ERROR("Decoder occurred an error, code: {}", errorCode);
    auto event = std::make_shared<DecoderEventArgs>(decoderName, streamIndex_, mediaType,
                                                    codecCtx_->hw_device_ctx != nullptr,
                                                    decoderName, description);
    eventDispatcher_->triggerEvent(EventType::kDecodeError, event);

    // 重置解码器
    avcodec_flush_buffers(codecCtx_);

    // 休眠，等待恢复
    std::this_thread::sleep_for(std::chrono::milliseconds(recoveryInterval_));

    return true;
}

bool DecoderBase::handleDecodeRecovery(const std::string &decoderName, MediaType mediaType,
                                       const std::string &description)
{
    auto event = std::make_shared<DecoderEventArgs>(decoderName, streamIndex_, mediaType,
                                                    codecCtx_->hw_device_ctx != nullptr,
                                                    decoderName, description);
    eventDispatcher_->triggerEvent(EventType::kDecodeRecovery, event);

    return true;
}

double DecoderBase::calculateFrameDisplayTime(
    double pts, double duration,
    std::optional<std::chrono::steady_clock::time_point> &lastFrameTime)
{
    if (std::isnan(pts)) {
        return 0.0;
    }

    // 获取当前播放速度
    double currentSpeed = speed();
    if (currentSpeed <= 0.0f) {
        currentSpeed = 1.0f; // 防止除零错误
    }

    // 首次调用，初始化
    auto currentTime = std::chrono::steady_clock::now();
    if (!lastFrameTime.has_value()) {
        lastFrameTime = currentTime;
        return 0.0;
    }

    // 基于帧率计算理论帧间隔，并考虑播放速度
    double frameInterval = duration;
    frameInterval /= currentSpeed; // 速度越快，帧间隔越短

    // 计算下一帧应该解码的时间点
    const auto nextFrameTime =
        *lastFrameTime + std::chrono::microseconds(static_cast<int64_t>(frameInterval * 1000.0));

    // 计算基本延迟时间
    double baseDelay =
        std::chrono::duration_cast<std::chrono::microseconds>(nextFrameTime - currentTime).count() /
        1000.0;

    // 更新上一帧时间
    lastFrameTime =
        currentTime + std::chrono::microseconds(static_cast<int64_t>(baseDelay * 1000.0));

    return std::max(0.0, baseDelay);
}

bool DecoderBase::checkAndUpdateSerial(int &currentSerial, PacketQueue *packetQueue)
{
    if (currentSerial != packetQueue->serial()) {
        avcodec_flush_buffers(codecCtx_);
        currentSerial = packetQueue->serial();
        frameQueue_->setSerial(currentSerial);
        return true; // 序列号发生了变化
    }
    return false; // 序列号没有变化
}

bool DecoderBase::shouldContinueDecoding() const
{
    return isRunning_.load() && statistics_.consecutiveErrors.load() < maxConsecutiveErrors_;
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END