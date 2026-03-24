#ifndef DECODER_SDK_INTERNAL_AUDIO_ENCODER_H
#define DECODER_SDK_INTERNAL_AUDIO_ENCODER_H

#include "encoder_base.h"

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
}

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class AudioEncoder : public EncoderBase {
public:
    explicit AudioEncoder(std::shared_ptr<Muxer> muxer);
    ~AudioEncoder() override;

    MediaType mediaType() const override
    {
        return MediaType::kAudio;
    }

    bool open(const EncoderConfig &config) override;
    bool getWritableFrame(Frame &frame) const override;

protected:
    void encodeLoop() override;

private:
    /**
     * @brief 音频重采样与编码管线状态
     *
     * - swrCtx_：重采样上下文，按需创建/更新
     * - resampleFrame_：用于承接重采样后的输出软帧（复用缓冲）
     * - audioFifo_：FIFO 缓冲，用于按编码器 frame_size 组织样本
     * - pts_：以样本数累加的时间戳（与 time_base = 1/sample_rate 对应）
     */
    SwrContext *swrCtx_ = nullptr;
    AVFrame *resampleFrame_ = nullptr;
    AVAudioFifo *audioFifo_ = nullptr;
    int64_t pts_ = 0;
    AVSampleFormat lastInputSampleFmt_ = AV_SAMPLE_FMT_NONE;
    int lastInputSampleRate_ = 0;
#if LIBAVCODEC_VERSION_MAJOR >= 60
    AVChannelLayout lastInputChLayout_{};
    bool hasLastInputLayout_ = false;
#else
    uint64_t lastInputChannelLayout_ = 0;
    int lastInputChannels_ = 0;
#endif
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_AUDIO_ENCODER_H
