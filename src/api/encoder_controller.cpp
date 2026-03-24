#include "include/decodersdk/encoder_controller.h"
#include "encoder_controller.h" // Internal header

namespace decoder_sdk {

EncoderController::EncoderController() : impl_(std::make_unique<internal::EncoderController>())
{
}

EncoderController::~EncoderController()
{
}

bool EncoderController::open(const std::string &url, const EncoderConfig &config)
{
    return impl_ ? impl_->open(url, config) : false;
}

void EncoderController::close()
{
    if (impl_)
        impl_->close();
}

void EncoderController::start()
{
    if (impl_)
        impl_->start();
}

void EncoderController::stop()
{
    if (impl_)
        impl_->stop();
}

Frame EncoderController::getWriteableFrame(MediaType mediaType) const
{
    if (!impl_)
        return {};

    auto internalFrame = std::make_unique<internal::Frame>();
    if (!impl_->getWriteableFrame(mediaType, *internalFrame)) {
        return {};
    }

    return Frame(std::move(internalFrame));
}

bool EncoderController::pushFrame(MediaType mediaType, const Frame &frame)
{
    if (!impl_ || !frame.isValid())
        return false;

    // Extract internal frame
    const internal::Frame *internalFrame = frame.impl_.get();
    if (internalFrame) {
        return impl_->pushFrame(mediaType, *internalFrame);
    }
    return false;
}

} // namespace decoder_sdk
