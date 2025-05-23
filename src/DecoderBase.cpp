#include "DecoderBase.h"

DecoderBase::DecoderBase(std::shared_ptr<Demuxer> demuxer, std::shared_ptr<SyncController> syncController)
    : demuxer_(demuxer)
    , syncController_(syncController)
    , frameRateControlEnabled_{true}
    , frameQueue_(3, true)
    , isRunning_(false)
    , speed_(1.0f)
    , seekPos_{0.0}
{
}

DecoderBase::~DecoderBase()
{
    stop();
    close();
}

bool DecoderBase::open()
{
    auto *const formatContext = demuxer_->formatContext();
    if (!formatContext)
    {
        return false;
    }

    streamIndex_ = demuxer_->streamIndex(type());
    if (streamIndex_ < 0)
    {
        // Todo: log error
        return false;
    }

    stream_ = formatContext->streams[streamIndex_];

    const AVCodec *codec = avcodec_find_decoder(stream_->codecpar->codec_id);
    if (!codec)
    {
        // Todo: log
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_)
    {
        // Todo: log
        return false;
    }

    if (avcodec_parameters_to_context(codecCtx_, stream_->codecpar) < 0)
    {
        // Todo: log
        return false;
    }
    // 尝试设置硬解
    setHardwareDecode();

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0)
    {
        // Todo: log
        return false;
    }

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
}

void DecoderBase::stop()
{
    if (!isRunning_)
        return;
    
    isRunning_ = false;
    frameQueue_.setAbortStatus(true);
    frameQueue_.awakeCond();

    sleepCond_.notify_all();
    
    if (thread_.joinable())
        thread_.join();
}

void DecoderBase::close()
{
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
    }
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
    if (speed <= 0.0f || std::abs(speed - speed_.load()) < std::numeric_limits<double>::epsilon()) {
        return false;
    }

    speed_.store(speed);
    syncController_->setSpeed(speed);

    return true;
}

double DecoderBase::calculatePts(AVFrame *frame) const
{
    const int64_t pts = (frame->pts != AV_NOPTS_VALUE) ?
                frame->pts :
                frame->best_effort_timestamp;  // av_frame_get_best_effort_timestamp(frame);
    const double time = pts * av_q2d(stream_->time_base);

    return time;
}