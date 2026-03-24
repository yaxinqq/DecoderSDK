#include "audio_encoder.h"
#include "logger/logger.h"
#include "utils/common_utils.h"
#include <iostream>
#include <limits>

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace {
const AVCodec *findEncoder(EncoderConfig::AudioCodec audioCodec)
{
    switch (audioCodec) {
        case EncoderConfig::AudioCodec::kAAC:
        default:
            break;
    }

    return avcodec_find_encoder(AV_CODEC_ID_AAC);
}

AVSampleFormat pickFirstSupportedSampleFormat(const AVCodec *codec, AVSampleFormat fallback)
{
    if (!codec) {
        return fallback;
    }
#if LIBAVCODEC_VERSION_MAJOR >= 61
    // FFmpeg 新版：通过 avcodec_get_supported_config 查询支持的 sample_fmts
    const AVSampleFormat *sampleFmts = nullptr;
    int numSampleFmts = 0;
    const int ret =
        avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                     reinterpret_cast<const void **>(&sampleFmts), &numSampleFmts);
    if (ret >= 0 && sampleFmts && numSampleFmts > 0 && sampleFmts[0] != AV_SAMPLE_FMT_NONE) {
        return sampleFmts[0];
    }
#endif
#if LIBAVCODEC_VERSION_MAJOR < 61
    // 旧版回退：直接读取 codec->sample_fmts
    if (codec->sample_fmts && codec->sample_fmts[0] != AV_SAMPLE_FMT_NONE) {
        return codec->sample_fmts[0];
    }
#endif
    return fallback;
}
} // namespace

AudioEncoder::AudioEncoder(std::shared_ptr<Muxer> muxer) : EncoderBase(muxer)
{
}

AudioEncoder::~AudioEncoder()
{
    if (swrCtx_) {
        swr_free(&swrCtx_);
        swrCtx_ = nullptr;
    }
#if LIBAVCODEC_VERSION_MAJOR >= 60
    if (hasLastInputLayout_) {
        av_channel_layout_uninit(&lastInputChLayout_);
        hasLastInputLayout_ = false;
    }
#endif
    if (resampleFrame_) {
        av_frame_free(&resampleFrame_);
        resampleFrame_ = nullptr;
    }
    if (audioFifo_) {
        av_audio_fifo_free(audioFifo_);
        audioFifo_ = nullptr;
    }
}

bool AudioEncoder::open(const EncoderConfig &config)
{
    if (isOpened_)
        return true;
    config_ = config;

    const AVCodec *codec = findEncoder(EncoderConfig::AudioCodec::kAAC);
    if (!codec) {
        LOG_ERROR("Audio codec not found");
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_)
        return false;

    codecCtx_->sample_rate = config.sampleRate;
    codecCtx_->ch_layout.nb_channels = config.channels;

#if LIBAVCODEC_VERSION_MAJOR >= 60
    av_channel_layout_default(&codecCtx_->ch_layout, config.channels);
#else
    codecCtx_->channels = config.channels;
    codecCtx_->channel_layout = av_get_default_channel_layout(config.channels);
#endif
    codecCtx_->bit_rate = config.audioBitrate;
    codecCtx_->sample_fmt = pickFirstSupportedSampleFormat(codec, AV_SAMPLE_FMT_FLTP);
    codecCtx_->time_base = {1, config.sampleRate};

    if (muxer_->isOpened()) {
        codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    int ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        LOG_ERROR("Could not open audio codec: {}", utils::avErr2Str(ret));
        return false;
    }

    streamIndex_ = muxer_->addStream(codecCtx_);
    if (streamIndex_ < 0) {
        LOG_ERROR("Could not add audio stream");
        return false;
    }

    // Allocate FIFO
    audioFifo_ = av_audio_fifo_alloc(codecCtx_->sample_fmt, codecCtx_->ch_layout.nb_channels, 1);
    if (!audioFifo_) {
        LOG_ERROR("Could not allocate audio FIFO");
        return false;
    }

    isOpened_ = true;
    LOG_INFO("Audio encoder opened: {}Hz, {}ch, {}", config.sampleRate, config.channels,
             codec->name);
    return true;
}

bool AudioEncoder::getWritableFrame(Frame &frame) const
{
    if (!codecCtx_)
        return false;

    // 确保帧有效
    frame.ensureAllocated();
    AVFrame *avFrame = frame.get();

    av_frame_make_writable(avFrame);
    int ret = 0;

    avFrame->nb_samples = codecCtx_->frame_size;
    avFrame->format = codecCtx_->sample_fmt;
    ret = av_channel_layout_copy(&avFrame->ch_layout, &codecCtx_->ch_layout);
    if (ret < 0) {
        LOG_WARN("Get frame buffer failed! Error code: {}, msg: {}", ret, utils::avErr2Str(ret));
        return false;
    }

    ret = av_frame_get_buffer(avFrame, 0);
    if (ret < 0) {
        LOG_WARN("Get frame buffer failed! Error code: {}, msg: {}", ret, utils::avErr2Str(ret));
        return false;
    }

    return true;
}

void AudioEncoder::encodeLoop()
{
    Frame frame;
    pts_ = 0;

    // 管线：输入帧 -> 按需重采样/格式转换 -> FIFO 写入 -> FIFO 读出（按 frame_size） -> 编码

    // Use a temporary frame for FIFO read
    AVFrame *fifoFrame = av_frame_alloc();
    if (!fifoFrame) {
        LOG_ERROR("Could not allocate fifo frame");
        return;
    }

    fifoFrame->nb_samples = codecCtx_->frame_size;
    fifoFrame->format = codecCtx_->sample_fmt;
#if LIBAVCODEC_VERSION_MAJOR >= 60
    av_channel_layout_copy(&fifoFrame->ch_layout, &codecCtx_->ch_layout);
#else
    fifoFrame->channel_layout = codecCtx_->channel_layout;
    fifoFrame->channels = codecCtx_->channels;
#endif
    fifoFrame->sample_rate = codecCtx_->sample_rate;

    // Pre-allocate buffer for fifoFrame
    int ret = av_frame_get_buffer(fifoFrame, 0);
    if (ret < 0) {
        LOG_ERROR("Could not allocate buffer for fifo frame: {}", utils::avErr2Str(ret));
        av_frame_free(&fifoFrame);
        return;
    }

    while (isRunning_) {
        if (frameQueue_->pop(frame, 100)) {
            if (!frame.isValid())
                continue;

            AVFrame *srcAVFrame = frame.get();
            AVFrame *convertedFrame = srcAVFrame;

            // 1. Resample/Convert input frame to codec format if needed
            bool needResample =
                (srcAVFrame->format != codecCtx_->sample_fmt) ||
                (srcAVFrame->sample_rate != codecCtx_->sample_rate) ||
#if LIBAVCODEC_VERSION_MAJOR >= 60
                (av_channel_layout_compare(&srcAVFrame->ch_layout, &codecCtx_->ch_layout) != 0);
#else
                (srcAVFrame->channel_layout != codecCtx_->channel_layout);
#endif

            if (needResample) {
                bool inputChanged = false;
                if (!swrCtx_) {
                    inputChanged = true;
                } else if (lastInputSampleFmt_ != srcAVFrame->format ||
                           lastInputSampleRate_ != srcAVFrame->sample_rate) {
                    inputChanged = true;
                }
#if LIBAVCODEC_VERSION_MAJOR >= 60
                if (!inputChanged) {
                    if (!hasLastInputLayout_ ||
                        av_channel_layout_compare(&lastInputChLayout_, &srcAVFrame->ch_layout) !=
                            0) {
                        inputChanged = true;
                    }
                }
#else
                if (!inputChanged) {
                    if (lastInputChannelLayout_ != srcAVFrame->channel_layout ||
                        lastInputChannels_ != srcAVFrame->channels) {
                        inputChanged = true;
                    }
                }
#endif

                if (inputChanged) {
                    if (swrCtx_) {
                        swr_free(&swrCtx_);
                    }
                    int setupRet = swr_alloc_set_opts2(
                        &swrCtx_,
#if LIBAVCODEC_VERSION_MAJOR >= 60
                        &codecCtx_->ch_layout,
#else
                        &codecCtx_->channel_layout,
#endif
                        codecCtx_->sample_fmt, codecCtx_->sample_rate,
#if LIBAVCODEC_VERSION_MAJOR >= 60
                        &srcAVFrame->ch_layout,
#else
                        &srcAVFrame->channel_layout,
#endif
                        (AVSampleFormat)srcAVFrame->format, srcAVFrame->sample_rate, 0, nullptr);
                    if (setupRet < 0) {
                        LOG_ERROR("Resampler setup failed: {}", utils::avErr2Str(setupRet));
                        continue;
                    }
                    int initRet = swr_init(swrCtx_);
                    if (initRet < 0) {
                        LOG_ERROR("Resampler init failed: {}", utils::avErr2Str(initRet));
                        swr_free(&swrCtx_);
                        continue;
                    }

                    lastInputSampleFmt_ = static_cast<AVSampleFormat>(srcAVFrame->format);
                    lastInputSampleRate_ = srcAVFrame->sample_rate;
#if LIBAVCODEC_VERSION_MAJOR >= 60
                    if (hasLastInputLayout_) {
                        av_channel_layout_uninit(&lastInputChLayout_);
                    }
                    av_channel_layout_copy(&lastInputChLayout_, &srcAVFrame->ch_layout);
                    hasLastInputLayout_ = true;
#else
                    lastInputChannelLayout_ = srcAVFrame->channel_layout;
                    lastInputChannels_ = srcAVFrame->channels;
#endif
                }

                if (!resampleFrame_) {
                    resampleFrame_ = av_frame_alloc();
                }

                av_frame_unref(resampleFrame_);
                resampleFrame_->format = codecCtx_->sample_fmt;
#if LIBAVCODEC_VERSION_MAJOR >= 60
                av_channel_layout_copy(&resampleFrame_->ch_layout, &codecCtx_->ch_layout);
#else
                resampleFrame_->channel_layout = codecCtx_->channel_layout;
                resampleFrame_->channels = codecCtx_->channels;
#endif
                resampleFrame_->sample_rate = codecCtx_->sample_rate;

                int64_t delay =
                    swr_get_delay(swrCtx_, srcAVFrame->sample_rate) + srcAVFrame->nb_samples;
                const int64_t outSamples64 = av_rescale_rnd(delay, codecCtx_->sample_rate,
                                                            srcAVFrame->sample_rate, AV_ROUND_UP);
                if (outSamples64 <= 0 || outSamples64 > std::numeric_limits<int>::max()) {
                    LOG_ERROR("Resample failed: invalid output sample count");
                    continue;
                }
                const int outSamples = static_cast<int>(outSamples64);

                resampleFrame_->nb_samples = outSamples;
                ret = av_frame_get_buffer(resampleFrame_, 0);
                if (ret < 0) {
                    LOG_ERROR("Resample buffer alloc failed: {}", utils::avErr2Str(ret));
                    continue;
                }

                int converted = swr_convert(swrCtx_, resampleFrame_->extended_data, outSamples,
                                            (const uint8_t **)srcAVFrame->extended_data,
                                            srcAVFrame->nb_samples);
                if (converted < 0) {
                    LOG_ERROR("Resample failed: {}", utils::avErr2Str(converted));
                    continue;
                }
                resampleFrame_->nb_samples = converted;
                convertedFrame = resampleFrame_;
            }

            // 2. Write to FIFO
            int written = av_audio_fifo_write(audioFifo_, (void **)convertedFrame->extended_data,
                                              convertedFrame->nb_samples);
            if (written < convertedFrame->nb_samples) {
                LOG_WARN("Audio FIFO overflow or write error");
            }

            // 3. Read from FIFO in codec frame size chunks
            while (av_audio_fifo_size(audioFifo_) >= codecCtx_->frame_size) {
                ret = av_audio_fifo_read(audioFifo_, (void **)fifoFrame->extended_data,
                                         codecCtx_->frame_size);
                if (ret < codecCtx_->frame_size) {
                    LOG_ERROR("Could not read enough samples from FIFO");
                    break;
                }

                fifoFrame->pts = pts_;
                pts_ += fifoFrame->nb_samples;

                ret = sendFrameToCodec(fifoFrame);
                if (ret < 0) {
                    LOG_ERROR("Error sending audio frame to codec: {}", utils::avErr2Str(ret));
                }

                while (ret >= 0) {
                    ret = receivePacketAndMux();
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    } else if (ret < 0) {
                        LOG_ERROR("Error receiving audio packet from codec: {}",
                                  utils::avErr2Str(ret));
                        break;
                    }
                }
                statistics_.framesEncoded++;
            }
        } else {
            if (frameQueue_->size() == 0 && !isRunning_)
                break;
        }
    }

    // Flush FIFO
    if (av_audio_fifo_size(audioFifo_) > 0) {
        // Handle remaining samples?
        // Typically padding with silence or just sending what's left if codec supports it.
        // For AAC, usually needs fixed size. We might need to pad.
        int remaining = av_audio_fifo_size(audioFifo_);
        // Simple padding logic: read remaining, zero out the rest
        ret = av_audio_fifo_read(audioFifo_, (void **)fifoFrame->extended_data, remaining);
        if (ret > 0) {
            // Zero out the rest of the frame
            int samplesToPad = codecCtx_->frame_size - remaining;
            if (samplesToPad > 0) {
                // Mute the rest
                for (int ch = 0; ch < codecCtx_->ch_layout.nb_channels; ++ch) {
                    // Assuming planar float for now (AAC default)
                    float *data = (float *)fifoFrame->data[ch];
                    std::fill(data + remaining, data + codecCtx_->frame_size, 0.0f);
                }
            }
            fifoFrame->pts = pts_;
            sendFrameToCodec(fifoFrame);
            while (true) {
                ret = receivePacketAndMux();
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
            }
        }
    }

    // Flush encoder
    flush();

    av_frame_free(&fifoFrame);
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END
