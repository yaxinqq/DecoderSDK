#include "decoder_base.h"

#include <algorithm>
#include <thread>

#include "demuxer/demuxer.h"
#include "event_system/event_dispatcher.h"
#include "logger/logger.h"
#include "utils/common_utils.h"

namespace {
constexpr int kFrameQueueDefaultSize = 3;
} // namespace

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

DecoderBase::DecoderBase(std::shared_ptr<Demuxer> demuxer,
                         std::shared_ptr<EventDispatcher> eventDispatcher,
                         std::shared_ptr<SeekCoordinator> seekCoordinator)
    : demuxer_(demuxer),
      eventDispatcher_(eventDispatcher),
      seekCoordinator_(seekCoordinator),
      frameQueue_(new FrameQueue(kFrameQueueDefaultSize, false, false))
{
    statistics_.reset();
}

DecoderBase::~DecoderBase()
{
    close();
}

bool DecoderBase::open()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return openInternal();
}

void DecoderBase::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    startInternal();
}

void DecoderBase::stop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    stopInternal();
}

void DecoderBase::pause()
{
    std::lock_guard<std::mutex> lock(mutex_);
    pauseInternal();
}

void DecoderBase::resume()
{
    std::lock_guard<std::mutex> lock(mutex_);
    resumeInternal();
}

void DecoderBase::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    closeInternal();
}

std::shared_ptr<FrameQueue> DecoderBase::frameQueue()
{
    return frameQueue_;
}

double DecoderBase::seekPos() const
{
    if (!seekCoordinator_)
        return -1.0;
    auto state = seekCoordinator_->getState();
    return (state.phase != SeekPhase::kIdle) ? state.targetPosSec : -1.0;
}

bool DecoderBase::shouldDiscardBySeek(double pts, uint64_t serial) const
{
    if (!seekCoordinator_)
        return false;

    auto state = seekCoordinator_->getState();
    // 只有在已提交(Committed)阶段才需要抛帧
    if (state.phase != SeekPhase::kCommitted) {
        return false;
    }

    uint64_t targetSerial =
        (codecCtx_->codec_type == AVMEDIA_TYPE_VIDEO) ? state.videoSerial : state.audioSerial;

    // 如果序列号匹配，且 PTS 小于目标位置，则需要丢弃
    if (serial == targetSerial) {
        if (utils::less(pts, state.targetPosSec)) {
            return true;
        }
    }

    return false;
}

bool DecoderBase::setSpeed(double speed)
{
    if (speed <= 0.0f || utils::greater(speed, 64.0)) {
        return false;
    }

    utils::atomicUpdateIfNotEqual<uint16_t>(speed_, static_cast<uint16_t>(speed * 1000));
    return true;
}

double DecoderBase::speed() const
{
    return speed_ * 0.001;
}

void DecoderBase::setMaxFrameQueueSize(uint32_t size)
{
    frameQueue_->setMaxCount(size);
}

uint32_t DecoderBase::maxFrameQueueSize() const
{
    return frameQueue_->capacity();
}

void DecoderBase::setMaxConsecutiveErrors(uint16_t maxErrors)
{
    utils::atomicUpdateIfNotEqual<uint16_t>(maxConsecutiveErrors_, maxErrors);
}

uint16_t DecoderBase::maxConsecutiveErrors() const
{
    return maxConsecutiveErrors_.load();
}

void DecoderBase::setRecoveryInterval(uint16_t interval)
{
    utils::atomicUpdateIfNotEqual<uint16_t>(recoveryInterval_, interval);
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

AVMediaType DecoderBase::type() const
{
    return AVMEDIA_TYPE_UNKNOWN;
}

void DecoderBase::setWaitingForPreBuffer(bool waiting)
{
    utils::atomicUpdateIfNotEqual(waitingForPreBuffer_, waiting);
}

bool DecoderBase::isWaitingForPreBuffer() const
{
    return waitingForPreBuffer_.load();
}

HWAccelType DecoderBase::initHwAccelContext()
{
    return HWAccelType::kNone;
}

bool DecoderBase::setupHardwareDecode()
{
    return false;
}

bool DecoderBase::removeHardwareDecode()
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
    return utils::greaterAndEqual(time, 0.0) ? time : 0.0;
}

bool DecoderBase::handleFirstFrame()
{
    auto event =
        std::make_shared<DecoderEventArgs>(utils::avMediaType2MediaType(type()), decoderName(),
                                           utils::eventType2Desc(EventType::kDecodeFirstFrame));
    event->decoderInfo = decoderInfo();
    eventDispatcher_->triggerEvent(EventType::kDecodeFirstFrame, event);

    return true;
}

bool DecoderBase::handleDecodeError(int errorCode)
{
    if (errorCode == AVERROR(EOF) || errorCode == AVERROR(EAGAIN))
        return false;

    statistics_.errorsCount.fetch_add(1);
    LOG_WARN("{} Decoder occurred an error, code: {}", demuxer_->url(), errorCode);
    auto event = std::make_shared<DecoderEventArgs>(
        utils::avMediaType2MediaType(type()), decoderName(),
        utils::eventType2Desc(EventType::kDecodeError), errorCode, utils::avErr2Str(errorCode));
    eventDispatcher_->triggerEvent(EventType::kDecodeError, event);

    return true;
}

bool DecoderBase::handleDecodeTransError(int errorCode)
{
    statistics_.errorsCount.fetch_add(1);
    LOG_WARN("{} Decoder trans format occurred an error, code: {}", demuxer_->url(), errorCode);
    auto event =
        std::make_shared<DecoderEventArgs>(utils::avMediaType2MediaType(type()), decoderName(),
                                           utils::eventType2Desc(EventType::kDecodeTransError),
                                           errorCode, utils::avErr2Str(errorCode));
    eventDispatcher_->triggerEvent(EventType::kDecodeTransError, event);

    return true;
}

bool DecoderBase::handleDecodeRecovery()
{
    auto event =
        std::make_shared<DecoderEventArgs>(utils::avMediaType2MediaType(type()), decoderName(),
                                           utils::eventType2Desc(EventType::kDecodeRecovery));
    eventDispatcher_->triggerEvent(EventType::kDecodeRecovery, event);

    return true;
}

bool DecoderBase::checkAndUpdateSerial(uint64_t &currentSerial, PacketQueue *packetQueue)
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
    return !requestInterruption_.load() &&
           statistics_.consecutiveErrors.load() < maxConsecutiveErrors_;
}

bool DecoderBase::openInternal()
{
    if (isOpened_) {
        closeInternal();
    }

    const auto sendFailedEvent = [this]() {
        auto event = std::make_shared<DecoderEventArgs>(
            utils::avMediaType2MediaType(type()), decoderName(),
            utils::eventType2Desc(EventType::kCreateDecoderFailed));
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

    // 构建硬件加速上下文（如果需要）
    const auto hwType = initHwAccelContext();

    const AVCodec *codec = avcodec_find_decoder(stream_->codecpar->codec_id);
    // 如果当前的硬件加速上下文是qsv或是amf，则需要重新查找codec
    switch (hwType) {
#ifdef QSV_AVAILABLE
        case HWAccelType::kQsv:
            codec = avcodec_find_decoder_by_name(fmt::format("{}_qsv", codec->name).c_str());
            break;
#endif
#ifdef AMF_AVAILABLE
        case HWAccelType::kAmf:
            codec = avcodec_find_decoder_by_name(fmt::format("{}_amf", codec->name).c_str());
            break;
#endif
        default:
            break;
    }

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

    frameQueue_->init();

    // 发送解码器创建成功的事件
    auto event =
        std::make_shared<DecoderEventArgs>(utils::avMediaType2MediaType(type()), decoderName(),
                                           utils::eventType2Desc(EventType::kCreateDecoderSuccess));
    eventDispatcher_->triggerEvent(EventType::kCreateDecoderSuccess, event);

    isOpened_ = true;
    statistics_.reset();
    return true;
}

void DecoderBase::closeInternal()
{
    stopInternal();
    if (!isOpened_)
        return;

    if (codecCtx_) {
        // 显式释放硬件设备上下文
        if (codecCtx_->hw_device_ctx) {
            av_buffer_unref(&codecCtx_->hw_device_ctx);
            codecCtx_->hw_device_ctx = nullptr;
        }

        // 显式释放硬件帧上下文
        if (codecCtx_->hw_frames_ctx) {
            av_buffer_unref(&codecCtx_->hw_frames_ctx);
            codecCtx_->hw_frames_ctx = nullptr;
        }

        // 刷新解码器缓冲区，确保所有硬件帧都被释放
        avcodec_flush_buffers(codecCtx_);

        avcodec_free_context(&codecCtx_);
    }

    // 清空帧队列
    frameQueue_->uninit();

    // 清理硬解
    removeHardwareDecode();

    isOpened_ = false;

    // 发送解码已销毁的事件
    auto event =
        std::make_shared<DecoderEventArgs>(utils::avMediaType2MediaType(type()), decoderName(),
                                           utils::eventType2Desc(EventType::kDestoryDecoder));
    eventDispatcher_->triggerEvent(EventType::kDestoryDecoder, event);
}

void DecoderBase::startInternal()
{
    if (isStarted_)
        return;

    // 获取对应的包队列
    auto packetQueue = demuxer_->packetQueue(type());
    if (!packetQueue)
        return;

    // 设置帧队列的序列号与包队列一致
    frameQueue_->setSerial(packetQueue->serial());
    // 设置帧队列的中止状态与包队列一致
    frameQueue_->setAbortStatus(packetQueue->isAborted());

    requestInterruption_.store(false);

    // 绑定 PacketQueue 的 flush 回调，用于联动唤醒 FrameQueue
    // 使用 weak_ptr 确保回调安全，即使 DecoderBase 提前销毁也不会发生崩溃
    packetQueue->setFlushCallback([weakFq = std::weak_ptr<FrameQueue>(frameQueue_)]() {
        if (auto fq = weakFq.lock()) {
            fq->clear();
        }
    });

    thread_ = std::thread(&DecoderBase::decodeLoop, this);

    isStarted_ = true;

    // 发送解码已开始的事件
    auto event =
        std::make_shared<DecoderEventArgs>(utils::avMediaType2MediaType(type()), decoderName(),
                                           utils::eventType2Desc(EventType::kDecodeStarted));
    eventDispatcher_->triggerEvent(EventType::kDecodeStarted, event);
}

void DecoderBase::stopInternal()
{
    if (!isStarted_)
        return;

    // 清理回调，防止停止后仍被唤醒
    if (auto packetQueue = demuxer_->packetQueue(type())) {
        packetQueue->setFlushCallback(nullptr);
    }

    isPaused_.store(false);
    requestInterruption_.store(true);
    frameQueue_->setAbortStatus(true);

    // 唤醒暂停的线程
    pauseCv_.notify_all();

    if (thread_.joinable()) {
        thread_.join();
    }
    isStarted_ = false;

    // 发送解码已停止的事件
    auto event =
        std::make_shared<DecoderEventArgs>(utils::avMediaType2MediaType(type()), decoderName(),
                                           utils::eventType2Desc(EventType::kDecodeStopped));
    eventDispatcher_->triggerEvent(EventType::kDecodeStopped, event);
}

void DecoderBase::pauseInternal()
{
    if (!isOpened_ || !isStarted_ || isPaused_.load())
        return;

    isPaused_.store(true);

    // 发送解码已暂停的事件
    auto event =
        std::make_shared<DecoderEventArgs>(utils::avMediaType2MediaType(type()), decoderName(),
                                           utils::eventType2Desc(EventType::kDecodePaused));
    eventDispatcher_->triggerEvent(EventType::kDecodePaused, event);
}

void DecoderBase::resumeInternal()
{
    if (!isOpened_ || !isStarted_ || !isPaused_.load())
        return;

    isPaused_.store(false);
    pauseCv_.notify_all();

    // 发送解码已开始的事件
    auto event =
        std::make_shared<DecoderEventArgs>(utils::avMediaType2MediaType(type()), decoderName(),
                                           utils::eventType2Desc(EventType::kDecodeStarted));
    eventDispatcher_->triggerEvent(EventType::kDecodeStarted, event);
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END