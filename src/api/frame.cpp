#include "include/decodersdk/frame.h"

#include "base/frame.h"
#include "utils/common_utils.h"

namespace decoder_sdk {
Frame::Frame()
{
}

Frame::Frame(std::unique_ptr<internal::Frame> frame) : impl_(std::move(frame))
{
}

Frame::~Frame() = default;

Frame::Frame(Frame &&other) noexcept = default;

Frame &Frame::operator=(Frame &&other) noexcept = default;

bool Frame::isValid() const
{
    return impl_ ? impl_->isValid() : false;
}

double Frame::durationByFps() const
{
    return impl_ ? impl_->durationByFps() : 0.0;
}

bool Frame::isInHardware() const
{
    return impl_ ? impl_->isInHardware() : false;
}

double Frame::secPts() const
{
    return impl_ ? impl_->secPts() : 0.0;
}

int Frame::width() const
{
    return impl_ ? impl_->width() : 0;
}

int Frame::height() const
{
    return impl_ ? impl_->height() : 0;
}

ImageFormat Frame::pixelFormat() const
{
    return impl_ ? internal::utils::avPixelFormat2ImageFormat(impl_->pixelFormat())
                 : ImageFormat::kUnknown;
}

int64_t Frame::avPts() const
{
    return impl_ ? impl_->avPts() : 0;
}

int64_t Frame::pktDts() const
{
    return impl_ ? impl_->pktDts() : 0;
}

int Frame::keyFrame() const
{
    return impl_ ? impl_->keyFrame() : 0;
}

int64_t Frame::bestEffortTimestamp() const
{
    return impl_ ? impl_->bestEffortTimestamp() : 0;
}

int Frame::sampleRate() const
{
    return impl_ ? impl_->sampleRate() : 0;
}

int64_t Frame::nbSamples() const
{
    return impl_ ? impl_->nbSamples() : 0;
}

uint8_t *Frame::data(int plane) const
{
    return impl_ ? impl_->data(plane) : nullptr;
}

int Frame::linesize(int plane) const
{
    return impl_ ? impl_->linesize(plane) : 0;
}

bool Frame::isAudioFrame() const
{
    return impl_ ? impl_->isAudioFrame() : false;
}

bool Frame::isVideoFrame() const
{
    return impl_ ? impl_->isVideoFrame() : false;
}

int Frame::getBufferSize() const
{
    return impl_ ? impl_->getBufferSize() : 0;
}
} // namespace decoder_sdk