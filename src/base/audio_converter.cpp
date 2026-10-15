/**
 * @file audio_converter.cpp
 * @brief 实现 DecoderSDK 内部音频格式转换与重采样功能。
 *
 * 本文件封装 libswresample 的资源管理和转换流程，避免 FFmpeg 类型
 * 暴露到公开 API。实现同时兼容新旧版本的 AVChannelLayout 接口。
 */

#include "audio_converter.h"

// FFmpeg 头文件使用 C 接口声明，需要放在 extern "C" 中。
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <limits>

/**
 * @brief 本文件私有的格式映射和元数据处理辅助函数。
 */
namespace {

/**
 * @brief 将 SDK 音频样本格式转换为 FFmpeg 样本格式。
 *
 * @param format SDK 音频样本格式。
 * @return 对应的 FFmpeg 样本格式；无法识别时返回 AV_SAMPLE_FMT_NONE。
 */
AVSampleFormat toAVSampleFormat(decoder_sdk::AudioSampleFormat format)
{
    using decoder_sdk::AudioSampleFormat;

    switch (format) {
        case AudioSampleFormat::kFmtU8:
            return AV_SAMPLE_FMT_U8;
        case AudioSampleFormat::kFmtS16:
            return AV_SAMPLE_FMT_S16;
        case AudioSampleFormat::kFmtS32:
            return AV_SAMPLE_FMT_S32;
        case AudioSampleFormat::kFmtFlt:
            return AV_SAMPLE_FMT_FLT;
        case AudioSampleFormat::kFmtDbl:
            return AV_SAMPLE_FMT_DBL;
        case AudioSampleFormat::kFmtU8P:
            return AV_SAMPLE_FMT_U8P;
        case AudioSampleFormat::kFmtS16P:
            return AV_SAMPLE_FMT_S16P;
        case AudioSampleFormat::kFmtS32P:
            return AV_SAMPLE_FMT_S32P;
        case AudioSampleFormat::kFmtFltP:
            return AV_SAMPLE_FMT_FLTP;
        case AudioSampleFormat::kFmtDblP:
            return AV_SAMPLE_FMT_DBLP;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        case AudioSampleFormat::kFmtS64:
            return AV_SAMPLE_FMT_S64;
        case AudioSampleFormat::kFmtS64P:
            return AV_SAMPLE_FMT_S64P;
#endif
        default:
            return AV_SAMPLE_FMT_NONE;
    }
}

/**
 * @brief 将输入音频帧的业务元数据复制到转换后的音频帧。
 *
 * 重采样会生成新的 AVFrame，因此需要显式保留序列号、时间戳、媒体类型、
 * SEI 数据和硬件后端信息，确保后续渲染和时钟逻辑仍可正确使用。
 *
 * @param source 原始输入帧。
 * @param target 新生成的输出帧。
 */
void copyAudioFrameMetadata(const decoder_sdk::internal::Frame &source,
                            decoder_sdk::internal::Frame &target)
{
    target.setSerial(source.serial());
    target.setDurationByFps(source.durationByFps());
    target.setSecPts(source.secPts());
    target.setMediaType(source.mediaType());
    target.setUserSEIDataList(source.userSEIDataList());
    target.setBackendHwType(source.backendHwType());
    target.setAvPts(source.avPts());
    target.setPktDts(source.pktDts());
    target.setBestEffortTimestamp(source.bestEffortTimestamp());
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    target.setTimeBase(source.timeBase());
#endif
}

} // 匿名命名空间

namespace decoder_sdk {
namespace internal {

/**
 * @brief AudioConverter 的内部实现。
 *
 * 负责保存 FFmpeg 重采样上下文、目标格式和最近一次输入格式签名。
 * 当输入采样率、声道数、样本格式或倍速发生变化时，需要重新创建 SwrContext。
 */
struct AudioConverter::Impl {
    /**
     * @brief 析构时释放重采样上下文和声道布局资源。
     */
    ~Impl()
    {
        resetContext();
    }

    /**
     * @brief 配置目标音频格式。
     *
     * @param sampleRate 目标采样率。
     * @param channels 目标声道数。
     * @param format 目标样本格式。
     * @return 配置是否成功。
     */
    bool configure(int sampleRate, int channels, AudioSampleFormat format)
    {
        const AVSampleFormat targetFormat = toAVSampleFormat(format);
        // 提前拒绝无效参数，避免创建不可用的重采样上下文。
        if (sampleRate <= 0 || channels <= 0 || targetFormat == AV_SAMPLE_FMT_NONE) {
            return false;
        }

        // 目标格式未变化时保留现有上下文，避免不必要的重建。
        if (targetValid && outputSampleRate == sampleRate && outputChannels == channels &&
            outputFormat == targetFormat) {
            return true;
        }

        resetContext();
        outputSampleRate = sampleRate;
        outputChannels = channels;
        outputFormat = targetFormat;
        targetValid = true;
        return true;
    }

    /**
     * @brief 设置用于重采样的播放倍速。
     *
     * @param newSpeed 新的播放倍速。
     * @return 设置是否成功。
     */
    bool setSpeed(double newSpeed)
    {
        // 倍速会参与有效输入采样率计算，因此必须是有限且合理的正数。
        if (!std::isfinite(newSpeed) || newSpeed <= 0.0 || newSpeed > 64.0) {
            return false;
        }

        // 微小变化不触发上下文重建，避免频繁切换倍速导致音频抖动。
        if (std::abs(speed - newSpeed) < 0.0001) {
            return true;
        }

        speed = newSpeed;
        resetContext();
        return true;
    }

    /**
     * @brief 释放重采样上下文，保留目标格式配置。
     */
    void reset()
    {
        resetContext();
    }

    /**
     * @brief 确保当前输入帧对应的重采样上下文可用。
     *
     * @param source 输入 FFmpeg 音频帧。
     * @return 上下文已就绪返回 true，否则返回 false。
     */
    bool ensureContext(const AVFrame *source)
    {
        // 目标格式未配置，或输入帧没有可转换的音频数据时不能建立上下文。
        if (!targetValid || !source || source->sample_rate <= 0 || source->nb_samples <= 0) {
            return false;
        }

        // 输入格式和声道数直接取自 FFmpeg，后续用于判断是否需要重建上下文。
        const AVSampleFormat sourceFormat = static_cast<AVSampleFormat>(source->format);
        const int sourceChannels =
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
            source->ch_layout.nb_channels;
#else
            source->channels;
#endif
        if (sourceChannels <= 0 || sourceFormat == AV_SAMPLE_FMT_NONE) {
            return false;
        }

        // 倍速通过调整输入端有效采样率实现：
        // 输入采样率扩大 N 倍，再重采样回目标采样率，即可让音频以 N 倍速播放。
        const double scaledRate = static_cast<double>(source->sample_rate) * speed;
        if (scaledRate <= 0.0 ||
            scaledRate > static_cast<double>((std::numeric_limits<int>::max)())) {
            return false;
        }
        const int effectiveInputSampleRate = static_cast<int>(std::llround(scaledRate));

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        // 新版本 FFmpeg 使用 AVChannelLayout 描述声道布局。
        AVChannelLayout sourceLayout = {};
        if (source->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_default(&sourceLayout, sourceChannels);
        } else if (av_channel_layout_copy(&sourceLayout, &source->ch_layout) < 0) {
            return false;
        }

        // 输入格式签名完全相同，且上下文已存在时直接复用 SwrContext。
        const bool sameInput =
            context && inputSignatureValid && inputSampleRate == source->sample_rate &&
            inputNominalSampleRate == effectiveInputSampleRate &&
            inputChannels == sourceChannels && inputFormat == sourceFormat &&
            av_channel_layout_compare(&inputLayout, &sourceLayout) == 0;
        if (sameInput) {
            av_channel_layout_uninit(&sourceLayout);
            return true;
        }

        // 目标声道使用 FFmpeg 默认布局，再据此创建新的重采样上下文。
        AVChannelLayout outputLayout = {};
        av_channel_layout_default(&outputLayout, outputChannels);
        SwrContext *newContext = nullptr;
        const int allocResult =
            swr_alloc_set_opts2(&newContext, &outputLayout, outputFormat, outputSampleRate,
                                &sourceLayout, sourceFormat, effectiveInputSampleRate, 0, nullptr);
        av_channel_layout_uninit(&outputLayout);
        if (allocResult < 0 || !newContext || swr_init(newContext) < 0) {
            swr_free(&newContext);
            av_channel_layout_uninit(&sourceLayout);
            return false;
        }

        // 新上下文创建成功后，替换旧上下文并记录本次输入格式签名。
        resetContext();
        if (av_channel_layout_copy(&inputLayout, &sourceLayout) < 0) {
            swr_free(&newContext);
            av_channel_layout_uninit(&sourceLayout);
            return false;
        }
        context = newContext;
        inputSampleRate = source->sample_rate;
        inputNominalSampleRate = effectiveInputSampleRate;
        inputChannels = sourceChannels;
        inputFormat = sourceFormat;
        inputSignatureValid = true;
        av_channel_layout_uninit(&sourceLayout);
        return true;
#else
        // 旧版本 FFmpeg 使用位掩码描述声道布局。
        const uint64_t sourceLayout =
            source->channel_layout ? source->channel_layout
                                   : av_get_default_channel_layout(sourceChannels);
        // 输入格式和倍速均未变化时复用已有上下文。
        const bool sameInput =
            context && inputSignatureValid && inputSampleRate == source->sample_rate &&
            inputNominalSampleRate == effectiveInputSampleRate &&
            inputChannels == sourceChannels && inputFormat == sourceFormat &&
            inputLayout == sourceLayout;
        if (sameInput) {
            return true;
        }

        const uint64_t outputLayout = av_get_default_channel_layout(outputChannels);
        SwrContext *newContext =
            swr_alloc_set_opts(nullptr, outputLayout, outputFormat, outputSampleRate, sourceLayout,
                               sourceFormat, effectiveInputSampleRate, 0, nullptr);
        if (!newContext || swr_init(newContext) < 0) {
            swr_free(&newContext);
            return false;
        }

        // 替换旧上下文并缓存输入格式签名，供下一次转换复用判断。
        resetContext();
        context = newContext;
        inputSampleRate = source->sample_rate;
        inputNominalSampleRate = effectiveInputSampleRate;
        inputChannels = sourceChannels;
        inputFormat = sourceFormat;
        inputLayout = sourceLayout;
        inputSignatureValid = true;
        return true;
#endif
    }

    /**
     * @brief 释放 SwrContext 和输入声道布局，并清空输入格式签名。
     */
    void resetContext()
    {
        swr_free(&context);
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        av_channel_layout_uninit(&inputLayout);
        inputLayout = AVChannelLayout();
#else
        inputLayout = 0;
#endif
        inputSampleRate = 0;
        inputNominalSampleRate = 0;
        inputChannels = 0;
        inputFormat = AV_SAMPLE_FMT_NONE;
        inputSignatureValid = false;
    }

    // FFmpeg 重采样上下文，nullptr 表示尚未创建。
    SwrContext *context = nullptr;

    // 是否已成功配置目标格式。
    bool targetValid = false;

    // 当前播放倍速，默认 1.0。
    double speed = 1.0;

    // 目标音频参数。
    int outputSampleRate = 0;
    int outputChannels = 0;
    AVSampleFormat outputFormat = AV_SAMPLE_FMT_NONE;

    // 最近一次输入音频的格式签名，用于判断能否复用 SwrContext。
    bool inputSignatureValid = false;
    int inputSampleRate = 0;
    int inputNominalSampleRate = 0;
    int inputChannels = 0;
    AVSampleFormat inputFormat = AV_SAMPLE_FMT_NONE;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    // 新版本 FFmpeg 的输入声道布局。
    AVChannelLayout inputLayout = {};
#else
    // 旧版本 FFmpeg 的输入声道布局位掩码。
    uint64_t inputLayout = 0;
#endif
};

/**
 * @brief 创建内部音频转换器及其实现对象。
 */
AudioConverter::AudioConverter() : impl_(std::make_unique<Impl>())
{
}

/**
 * @brief 销毁内部音频转换器。
 */
AudioConverter::~AudioConverter() = default;

/**
 * @brief 移动构造内部音频转换器。
 */
AudioConverter::AudioConverter(AudioConverter &&other) noexcept = default;

/**
 * @brief 移动赋值内部音频转换器。
 */
AudioConverter &AudioConverter::operator=(AudioConverter &&other) noexcept = default;

/**
 * @brief 配置目标音频格式。
 */
bool AudioConverter::configure(int sampleRate, int channels, AudioSampleFormat format)
{
    return impl_ && impl_->configure(sampleRate, channels, format);
}

/**
 * @brief 设置播放倍速。
 */
bool AudioConverter::setSpeed(double speed)
{
    return impl_ && impl_->setSpeed(speed);
}

/**
 * @brief 转换为新的内部音频帧。
 *
 * 输出帧使用相同的音频业务元数据，但持有重新分配和重采样后的音频数据。
 */
Frame AudioConverter::convert(const Frame &input)
{
    // 转换器未初始化或输入帧无效时，返回无效帧。
    if (!impl_ || !input.isValid()) {
        return Frame();
    }

    // 根据输入格式和倍速准备或复用 libswresample 上下文。
    AVFrame *source = input.get();
    if (!source || !impl_->ensureContext(source)) {
        return Frame();
    }

    // 创建内部 Frame，并预分配一个可供 FFmpeg 写入的 AVFrame。
    auto converted = std::make_unique<internal::Frame>();
    converted->ensureAllocated();
    AVFrame *output = converted->get();
    if (!output) {
        return Frame();
    }

    // 计算输出样本数时必须计入重采样器内部延迟，否则跨帧转换可能丢失尾部样本。
    const int64_t delay =
        std::max<int64_t>(0, swr_get_delay(impl_->context, impl_->inputNominalSampleRate));
    const int64_t outputSamples =
        av_rescale_rnd(delay + source->nb_samples, impl_->outputSampleRate,
                       impl_->inputNominalSampleRate, AV_ROUND_UP);
    if (outputSamples <= 0 || outputSamples > (std::numeric_limits<int>::max)()) {
        return Frame();
    }

    // 设置输出帧的采样格式、采样率和声道布局。
    output->format = impl_->outputFormat;
    output->sample_rate = impl_->outputSampleRate;
    output->nb_samples = static_cast<int>(outputSamples);
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    AVChannelLayout outputLayout = {};
    av_channel_layout_default(&outputLayout, impl_->outputChannels);
    if (av_channel_layout_copy(&output->ch_layout, &outputLayout) < 0) {
        av_channel_layout_uninit(&outputLayout);
        return Frame();
    }
    av_channel_layout_uninit(&outputLayout);
#else
    output->channel_layout = av_get_default_channel_layout(impl_->outputChannels);
    output->channels = impl_->outputChannels;
#endif

    // 为输出帧申请真实的音频数据缓冲区。
    if (av_frame_get_buffer(output, 0) < 0) {
        return Frame();
    }

    // 平面格式需要为每个声道提供独立数据指针，交错格式只需要第一个指针。
    const uint8_t *inputData[AV_NUM_DATA_POINTERS] = {};
    const int inputPlanes = av_sample_fmt_is_planar(impl_->inputFormat) ? impl_->inputChannels : 1;
    for (int plane = 0; plane < inputPlanes && plane < AV_NUM_DATA_POINTERS; ++plane) {
        inputData[plane] = source->data[plane];
    }

    // 执行实际重采样，返回值表示成功写入的输出样本数。
    const int convertedSamples =
        swr_convert(impl_->context, output->data, static_cast<int>(outputSamples), inputData,
                    source->nb_samples);
    if (convertedSamples < 0) {
        return Frame();
    }

    // 修正实际输出样本数，并保留输入帧携带的业务元数据。
    output->nb_samples = convertedSamples;
    copyAudioFrameMetadata(input, *converted);
    return Frame(std::move(*converted));
}

/**
 * @brief 转换为交错 PCM 缓冲区，供 Qt 音频输出等调用方直接使用。
 *
 * 该重载不支持平面输出，因为调用方期望单一连续缓冲区。
 */
bool AudioConverter::convert(const Frame &input, std::vector<uint8_t> &output, int &sampleCount)
{
    // 无论成功还是失败，都先保证输出参数处于明确的初始状态。
    sampleCount = 0;
    output.clear();

    // 平面输出无法表示为单个连续 PCM 缓冲区，因此直接拒绝。
    if (!impl_ || !input.isValid() ||
        av_sample_fmt_is_planar(impl_->outputFormat)) {
        return false;
    }

    // 根据输入格式和倍速准备或复用 libswresample 上下文。
    AVFrame *source = input.get();
    if (!source || !impl_->ensureContext(source)) {
        return false;
    }

    // 计算包含重采样延迟在内的最大输出样本数。
    const int64_t delay =
        std::max<int64_t>(0, swr_get_delay(impl_->context, impl_->inputNominalSampleRate));
    const int64_t outputSamples =
        av_rescale_rnd(delay + source->nb_samples, impl_->outputSampleRate,
                       impl_->inputNominalSampleRate, AV_ROUND_UP);
    if (outputSamples <= 0 || outputSamples > (std::numeric_limits<int>::max)()) {
        return false;
    }

    // 先计算交错输出所需字节数，并一次性调整调用方缓冲区。
    const int bufferSize =
        av_samples_get_buffer_size(nullptr, impl_->outputChannels, static_cast<int>(outputSamples),
                                   impl_->outputFormat, 1);
    if (bufferSize <= 0) {
        return false;
    }

    output.resize(static_cast<size_t>(bufferSize));

    // 收集输入平面指针。交错输入只有一个平面，平面输入则每个声道一个指针。
    const uint8_t *inputData[AV_NUM_DATA_POINTERS] = {};
    const int inputPlanes = av_sample_fmt_is_planar(impl_->inputFormat) ? impl_->inputChannels : 1;
    for (int plane = 0; plane < inputPlanes && plane < AV_NUM_DATA_POINTERS; ++plane) {
        inputData[plane] = source->data[plane];
    }

    // 交错输出只需要一个连续缓冲区指针。
    uint8_t *outputData[1] = { output.data() };
    const int convertedSamples =
        swr_convert(impl_->context, outputData, static_cast<int>(outputSamples), inputData,
                    source->nb_samples);
    if (convertedSamples < 0) {
        output.clear();
        return false;
    }

    // 按实际输出样本数收缩缓冲区，避免把预估值留下的尾部字节交给调用方。
    const int convertedSize =
        av_samples_get_buffer_size(nullptr, impl_->outputChannels, convertedSamples,
                                   impl_->outputFormat, 1);
    if (convertedSize <= 0) {
        output.clear();
        return false;
    }

    output.resize(static_cast<size_t>(convertedSize));
    sampleCount = convertedSamples;
    return true;
}

/**
 * @brief 释放当前重采样上下文，保留目标格式配置。
 */
void AudioConverter::reset()
{
    // 重置实现层保存的重采样上下文。
    if (impl_) {
        impl_->reset();
    }
}

} // 内部命名空间
} // DecoderSDK 命名空间
