#include "include/decodersdk/audio_converter.h"

#include "base/audio_converter.h"

#include <utility>

namespace decoder_sdk {

AudioConverter::AudioConverter() : impl_(std::make_unique<internal::AudioConverter>())
{
}

AudioConverter::~AudioConverter() = default;

AudioConverter::AudioConverter(AudioConverter &&other) noexcept = default;

AudioConverter &AudioConverter::operator=(AudioConverter &&other) noexcept = default;

bool AudioConverter::configure(int sampleRate, int channels, AudioSampleFormat format)
{
    return impl_ && impl_->configure(sampleRate, channels, format);
}

bool AudioConverter::setSpeed(double speed)
{
    return impl_ && impl_->setSpeed(speed);
}

Frame AudioConverter::convert(const Frame &input)
{
    if (!impl_ || !input.impl_) {
        return Frame();
    }

    auto converted = impl_->convert(*input.impl_);
    return Frame(std::make_unique<internal::Frame>(std::move(converted)));
}

bool AudioConverter::convert(const Frame &input, std::vector<uint8_t> &output, int &sampleCount)
{
    if (!impl_ || !input.impl_) {
        output.clear();
        sampleCount = 0;
        return false;
    }

    return impl_->convert(*input.impl_, output, sampleCount);
}

void AudioConverter::reset()
{
    if (impl_) {
        impl_->reset();
    }
}

} // namespace decoder_sdk
