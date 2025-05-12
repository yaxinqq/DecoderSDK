#include <memory>

#include "BaseDecoder.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

BaseDecoder::BaseDecoder()
    : pktQueue_{nullptr}
    , frameQueue_{nullptr}
    , codecCtx_{nullptr}
    , pktSerial_{0}
    , finished_{false}
    , packetPending_{false}
    , startPts_{AV_NOPTS_VALUE}
    , nextPts_{AV_NOPTS_VALUE}
    , emptyQueueCond_{nullptr}
{

}

BaseDecoder::~BaseDecoder()
{
    destroy();
}

int BaseDecoder::init(AVFormatContext *formatCtx, AVCodecContext *avctx, PacketQueue *packetQueue, FrameQueue *frameQueue, std::condition_variable *empty_queue_cond)
{
    pkt_.reset(av_packet_alloc());
    if (!pkt_) {
        return AVERROR(ENOMEM);
    }

    formatCtx_ = formatCtx;
    codecCtx_ = avctx;
    pktQueue_ = packetQueue;
    frameQueue_ = frameQueue;
    emptyQueueCond_ = empty_queue_cond;

    startPts_ = AV_NOPTS_VALUE;
    pktSerial_ = -1;
    return 0;
}

void BaseDecoder::abort()
{
    if (pktQueue_)  {
        pktQueue_->abort();
    }
    if (frameQueue_) {
        frameQueue_->awakeCond();
    }
    if (thread_.joinable()) {
        thread_.join();
    }

    if (pktQueue_)  {
        pktQueue_->flush();
    }
}

void BaseDecoder::destroy()
{
    pkt_.reset();
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
    }
    formatCtx_ = nullptr;

    pktQueue_ = nullptr;
    frameQueue_ = nullptr;
    emptyQueueCond_ = nullptr;

    startPts_ = AV_NOPTS_VALUE;
    pktSerial_ = -1;
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END