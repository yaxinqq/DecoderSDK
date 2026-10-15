#include "base/frame.h"
#include "include/decodersdk/audio_converter.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

namespace {

decoder_sdk::Frame makeFrame(AVSampleFormat format, int sampleRate, int channels,
                             int sampleCount, double secPts)
{
    auto frame = std::make_unique<decoder_sdk::internal::Frame>();
    frame->ensureAllocated();
    assert(frame->isValid());

    frame->setSampleFormat(format);
    frame->setSampleRate(sampleRate);
    frame->setNbSamples(sampleCount);
    frame->setAvPts(static_cast<int64_t>(secPts * 1000));
    frame->setPktDts(static_cast<int64_t>(secPts * 1000) - 1);
    frame->setSecPts(secPts);
    frame->setDurationByFps(0.25);

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    AVChannelLayout layout{};
    av_channel_layout_default(&layout, channels);
    frame->setChannelLayout(layout);
    av_channel_layout_uninit(&layout);
#else
    frame->setChannels(channels);
    frame->setChannelLayout(av_get_default_channel_layout(channels));
#endif

    assert(av_frame_get_buffer(frame->get(), 0) >= 0);

    if (av_sample_fmt_is_planar(format)) {
        for (int channel = 0; channel < channels; ++channel) {
            auto *samples = reinterpret_cast<int16_t *>(frame->data(channel));
            assert(samples);
            for (int index = 0; index < sampleCount; ++index) {
                samples[index] = static_cast<int16_t>((index + channel * 100) % 1000);
            }
        }
    } else {
        auto *samples = reinterpret_cast<int16_t *>(frame->data(0));
        assert(samples);
        for (int index = 0; index < sampleCount * channels; ++index) {
            samples[index] = static_cast<int16_t>(index % 1000);
        }
    }

    return decoder_sdk::Frame(std::move(frame));
}

void testInvalidConfiguration()
{
    decoder_sdk::AudioConverter converter;
    assert(!converter.configure(0, 2, decoder_sdk::AudioSampleFormat::kFmtS16));
    assert(!converter.configure(48000, 0, decoder_sdk::AudioSampleFormat::kFmtS16));
    assert(!converter.configure(48000, 2, decoder_sdk::AudioSampleFormat::kUnknown));
    assert(!converter.setSpeed(0.0));
    assert(!converter.setSpeed(-1.0));
    assert(!converter.setSpeed(65.0));
    assert(!converter.setSpeed(std::numeric_limits<double>::infinity()));
    std::cout << "testInvalidConfiguration passed\n";
}

void testInterleavedConversion()
{
    decoder_sdk::AudioConverter converter;
    assert(converter.configure(48000, 2, decoder_sdk::AudioSampleFormat::kFmtS16));

    const auto input = makeFrame(AV_SAMPLE_FMT_S16, 48000, 2, 1024, 12.5);
    const auto output = converter.convert(input);
    assert(output.isValid());
    assert(output.sampleRate() == 48000);
    assert(output.channels() == 2);
    assert(output.sampleFormat() == decoder_sdk::AudioSampleFormat::kFmtS16);
    assert(output.nbSamples() == 1024);
    assert(output.secPts() == 12.5);
    assert(output.durationByFps() == 0.25);
    assert(output.avPts() == 12500);
    std::cout << "testInterleavedConversion passed\n";
}

void testPlanarToInterleaved()
{
    decoder_sdk::AudioConverter converter;
    assert(converter.configure(44100, 2, decoder_sdk::AudioSampleFormat::kFmtS16));

    const auto input = makeFrame(AV_SAMPLE_FMT_S16P, 44100, 2, 512, 1.0);
    const auto output = converter.convert(input);
    assert(output.isValid());
    assert(output.sampleFormat() == decoder_sdk::AudioSampleFormat::kFmtS16);
    assert(output.nbSamples() == 512);
    std::cout << "testPlanarToInterleaved passed\n";
}

void testInterleavedToPlanar()
{
    decoder_sdk::AudioConverter converter;
    assert(converter.configure(48000, 2, decoder_sdk::AudioSampleFormat::kFmtS16P));

    const auto input = makeFrame(AV_SAMPLE_FMT_S16, 48000, 2, 512, 2.0);
    const auto output = converter.convert(input);
    assert(output.isValid());
    assert(output.sampleFormat() == decoder_sdk::AudioSampleFormat::kFmtS16P);
    assert(output.nbSamples() == 512);
    std::cout << "testInterleavedToPlanar passed\n";
}

void testBufferConversion()
{
    decoder_sdk::AudioConverter converter;
    assert(converter.configure(48000, 2, decoder_sdk::AudioSampleFormat::kFmtS16));

    const auto input = makeFrame(AV_SAMPLE_FMT_S16, 48000, 2, 1024, 3.0);
    std::vector<uint8_t> output;
    int sampleCount = 0;
    assert(converter.convert(input, output, sampleCount));
    assert(sampleCount == 1024);
    assert(output.size() == 1024U * 2U * sizeof(int16_t));

    assert(converter.configure(48000, 2, decoder_sdk::AudioSampleFormat::kFmtS16P));
    output.assign(1, 7);
    sampleCount = 9;
    assert(!converter.convert(input, output, sampleCount));
    assert(output.empty());
    assert(sampleCount == 0);
    std::cout << "testBufferConversion passed\n";
}

void testSpeedResampling()
{
    decoder_sdk::AudioConverter converter;
    assert(converter.configure(48000, 2, decoder_sdk::AudioSampleFormat::kFmtS16));
    assert(converter.setSpeed(2.0));

    const auto input = makeFrame(AV_SAMPLE_FMT_S16, 48000, 2, 4800, 4.0);
    const auto output = converter.convert(input);
    assert(output.isValid());
    assert(output.sampleRate() == 48000);
    assert(output.nbSamples() >= 2300);
    assert(output.nbSamples() <= 2420);
    assert(output.secPts() == 4.0);
    std::cout << "testSpeedResampling passed\n";
}

void testTargetReconfiguration()
{
    decoder_sdk::AudioConverter converter;
    assert(converter.configure(48000, 2, decoder_sdk::AudioSampleFormat::kFmtS16));
    const auto input = makeFrame(AV_SAMPLE_FMT_S16, 44100, 1, 441, 5.0);

    const auto first = converter.convert(input);
    assert(first.isValid());
    assert(first.sampleRate() == 48000);
    assert(first.channels() == 2);

    assert(converter.configure(44100, 1, decoder_sdk::AudioSampleFormat::kFmtFlt));
    const auto second = converter.convert(input);
    assert(second.isValid());
    assert(second.sampleRate() == 44100);
    assert(second.channels() == 1);
    assert(second.sampleFormat() == decoder_sdk::AudioSampleFormat::kFmtFlt);
    std::cout << "testTargetReconfiguration passed\n";
}

void testInvalidInput()
{
    decoder_sdk::AudioConverter converter;
    assert(converter.configure(48000, 2, decoder_sdk::AudioSampleFormat::kFmtS16));
    assert(!converter.convert(decoder_sdk::Frame()).isValid());
}

} // namespace

int main()
{
    testInvalidConfiguration();
    testInterleavedConversion();
    testPlanarToInterleaved();
    testInterleavedToPlanar();
    testBufferConversion();
    testSpeedResampling();
    testTargetReconfiguration();
    testInvalidInput();
    std::cout << "All AudioConverter tests passed!\n";
    return 0;
}
