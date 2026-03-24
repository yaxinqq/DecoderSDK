#include "encoder_base.h"
#include "logger/logger.h"
#include "utils/common_utils.h"
#include <iostream>

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

EncoderBase::EncoderBase(std::shared_ptr<Muxer> muxer) : muxer_(muxer)
{
    frameQueue_ = std::make_shared<FrameQueue>(30, false, true); // 30 frames buffer
}

EncoderBase::~EncoderBase()
{
    stop();
    close();
}

void EncoderBase::start()
{
    if (isRunning_ || !isOpened_)
        return;

    isRunning_ = true;
    thread_ = std::thread(&EncoderBase::encodeLoop, this);
    LOG_INFO("Encoder thread started");
}

void EncoderBase::stop()
{
    if (!isRunning_)
        return;

    LOG_INFO("Stopping encoder thread...");
    isRunning_ = false;
    frameQueue_->setAbortStatus(true);

    if (thread_.joinable()) {
        thread_.join();
    }
    LOG_INFO("Encoder thread stopped");
}

void EncoderBase::close()
{
    stop();

    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
    }
    isOpened_ = false;
    LOG_INFO("Encoder closed");
}

bool EncoderBase::pushFrame(const Frame &frame)
{
    if (!isOpened_)
        return false;
    return frameQueue_->push(frame);
}

void EncoderBase::flush()
{
    if (!codecCtx_ || !isOpened_)
        return;

    LOG_INFO("Flushing encoder...");
    // Send NULL frame to flush encoder
    int ret = sendFrameToCodec(nullptr);
    if (ret < 0) {
        LOG_ERROR("Error sending flush frame: {}", utils::avErr2Str(ret));
    }

    while (true) {
        ret = receivePacketAndMux();
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            LOG_ERROR("Error flushing packet: {}", utils::avErr2Str(ret));
            break;
        }
    }
    LOG_INFO("Encoder flushed");
}

const EncoderStatistics &EncoderBase::getStatistics() const
{
    return statistics_;
}

int EncoderBase::sendFrameToCodec(const AVFrame *frame)
{
    if (!codecCtx_)
        return -1;
    // 将一帧（或空帧用于 flush）送入编码器；非阻塞
    return avcodec_send_frame(codecCtx_, frame);
}

int EncoderBase::receivePacketAndMux()
{
    if (!codecCtx_ || !muxer_)
        return -1;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return -1;

    int ret = avcodec_receive_packet(codecCtx_, pkt);
    if (ret == 0) {
        pkt->stream_index = streamIndex_;

        // Rescale packet timestamp from codec timebase to stream timebase
        AVRational srcTB = codecCtx_->time_base;
        AVRational dstTB = muxer_->getStreamTimeBase(streamIndex_);

        av_packet_rescale_ts(pkt, srcTB, dstTB);

        // Update stats
        statistics_.packetsWritten++;

        // Packet takes ownership of AVPacket*
        Packet packet(pkt);

        if (!muxer_->writePacket(packet)) {
            LOG_ERROR("Failed to write packet");
            // If write fails, Packet destructor frees AVPacket
            return -1;
        }
    } else {
        // 未收到包或出错时释放临时 AVPacket
        av_packet_free(&pkt);
    }

    return ret;
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END
