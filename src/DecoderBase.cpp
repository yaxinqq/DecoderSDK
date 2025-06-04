#include "DecoderBase.h"

namespace {
MediaType transAVMediaType(AVMediaType type)
{
    switch (type) {
        case AVMEDIA_TYPE_VIDEO:
            return MediaType::kMediaTypeVideo;
        case AVMEDIA_TYPE_AUDIO:
            return MediaType::kMediaTypeAudio;
        default:
            return MediaType::kMediaTypeUnknown;
    }
}
}  // namespace

DecoderBase::DecoderBase(std::shared_ptr<Demuxer> demuxer,
                         std::shared_ptr<SyncController> syncController,
                         std::shared_ptr<EventDispatcher> eventDispatcher)
    : demuxer_(demuxer),
      syncController_(syncController),
      eventDispatcher_(eventDispatcher),
      frameRateControlEnabled_{true},
      frameQueue_(3, false),
      isRunning_(false),
      speed_(1.0f),
      seekPos_{0.0}
{
}

DecoderBase::~DecoderBase()
{
    stop();
    close();
}

bool DecoderBase::open()
{
    const auto sendFailedEvent = [this]() {
        auto event = std::make_shared<DecoderEventArgs>(
            codecCtx_ ? codecCtx_->codec->name : "", streamIndex_,
            transAVMediaType(type()), false, "Decoder",
            "Decode Created Failed");
        eventDispatcher_->triggerEventAsync(EventType::kCreateDecoderSuccess,
                                            event);
    };

    auto *const formatContext = demuxer_->formatContext();
    if (!formatContext) {
        sendFailedEvent();
        return false;
    }

    streamIndex_ = demuxer_->streamIndex(type());
    if (streamIndex_ < 0) {
        // Todo: log error
        sendFailedEvent();
        return false;
    }

    stream_ = formatContext->streams[streamIndex_];

    const AVCodec *codec = avcodec_find_decoder(stream_->codecpar->codec_id);
    if (!codec) {
        // Todo: log
        sendFailedEvent();
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        // Todo: log
        sendFailedEvent();
        return false;
    }

    if (avcodec_parameters_to_context(codecCtx_, stream_->codecpar) < 0) {
        // Todo: log
        sendFailedEvent();
        return false;
    }
    // 尝试设置硬解
    const bool useHw = setHardwareDecode();

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        // Todo: log
        sendFailedEvent();
        return false;
    }

    // 发送解码已开始的事件
    auto event = std::make_shared<DecoderEventArgs>(
        codecCtx_->codec->name, streamIndex_, transAVMediaType(type()), useHw,
        "Decoder", "Decode Created Success");
    eventDispatcher_->triggerEventAsync(EventType::kCreateDecoderSuccess,
                                        event);

    needClose_ = true;
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
    frameQueue_.setSerial(packetQueue->serial());
    // 设置帧队列的中止状态与包队列一致
    frameQueue_.setAbortStatus(packetQueue->isAbort());

    // 清空seek节点
    seekPos_.store(0.0);

    isRunning_ = true;
    thread_ = std::thread(&DecoderBase::decodeLoop, this);

    // 发送解码已开始的事件
    auto event = std::make_shared<DecoderEventArgs>(
        codecCtx_->codec->name, streamIndex_, transAVMediaType(type()),
        codecCtx_->hw_device_ctx != nullptr, "Decoder", "Decode Started");
    eventDispatcher_->triggerEventAsync(EventType::kDecodeStarted, event);
}

void DecoderBase::stop()
{
    if (!isRunning_)
        return;

    isRunning_ = false;
    frameQueue_.setAbortStatus(true);

    sleepCond_.notify_all();

    if (thread_.joinable())
        thread_.join();

    // 发送解码已停止的事件
    auto event = std::make_shared<DecoderEventArgs>(
        codecCtx_->codec->name, streamIndex_, transAVMediaType(type()),
        codecCtx_->hw_device_ctx != nullptr, "Decoder", "Decode Stopped");
    eventDispatcher_->triggerEventAsync(EventType::kDecodeStopped, event);
}

void DecoderBase::close()
{
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
        codecName, streamIndex_, transAVMediaType(type()), useHw, "Decoder",
        "Decode Destoryed");
    eventDispatcher_->triggerEventAsync(EventType::kDestoryDecoder, event);
}

void DecoderBase::setSeekPos(double pos)
{
    seekPos_.store(pos);
}

FrameQueue &DecoderBase::frameQueue()
{
    return frameQueue_;
}

bool DecoderBase::setSpeed(double speed)
{
    if (speed <= 0.0f || std::abs(speed - speed_.load()) <
                             std::numeric_limits<double>::epsilon()) {
        return false;
    }

    speed_.store(speed);
    syncController_->setSpeed(speed);

    return true;
}

double DecoderBase::calculatePts(AVFrame *frame) const
{
    const int64_t pts =
        (frame->pts != AV_NOPTS_VALUE)
            ? frame->pts
            : frame
                  ->best_effort_timestamp;  // av_frame_get_best_effort_timestamp(frame);
    const double time = pts * av_q2d(stream_->time_base);

    return time;
}