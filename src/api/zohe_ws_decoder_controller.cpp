#include "include/decodersdk/zohe_ws_decoder_controller.h"

#include "zohe_ws_decoder_controller.h"

extern "C" {
#include "libavcodec/codec_id.h"
}

namespace decoder_sdk {
ZoheWsDecoderController::ZoheWsDecoderController(const Config &config)
    : impl_{new internal::ZoheWsDecoderController{config}}
{
}

ZoheWsDecoderController::~ZoheWsDecoderController()
{
}

bool ZoheWsDecoderController::initDecoder(const std::string &enc, int width, int height,
                                          const uint8_t *extraData, int extraDataSize)
{
    AVCodecID codecID = (enc == "H265") ? AV_CODEC_ID_H265 : AV_CODEC_ID_H264;
    return impl_ ? impl_->initDecoder(codecID, width, height, extraData, extraDataSize) : false;
}

void ZoheWsDecoderController::setFrameCallback(
    std::function<void(const decoder_sdk::Frame &frame)> callback)
{
    if (!impl_)
        return;

    impl_->setFrameCallback(callback);
}

bool ZoheWsDecoderController::pushPacket(const uint8_t *data, int size)
{
    return impl_ ? impl_->pushPacket(data, size) : false;
}

void ZoheWsDecoderController::flush()
{
    if (!impl_)
        return;

    impl_->flush();
}

void ZoheWsDecoderController::cleanup()
{
    if (!impl_)
        return;

    impl_->cleanup();
}

bool ZoheWsDecoderController::isInitialized() const
{
    return impl_ ? impl_->isInitialized() : false;
}

} // namespace decoder_sdk