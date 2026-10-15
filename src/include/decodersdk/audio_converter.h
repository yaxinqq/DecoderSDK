#ifndef DECODER_SDK_AUDIO_CONVERTER_H
#define DECODER_SDK_AUDIO_CONVERTER_H

#include "common_define.h"
#include "frame.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace decoder_sdk {
namespace internal {
class AudioConverter;
}

class DECODER_SDK_API AudioConverter {
public:
    AudioConverter();
    ~AudioConverter();

    AudioConverter(AudioConverter &&other) noexcept;
    AudioConverter &operator=(AudioConverter &&other) noexcept;
    AudioConverter(const AudioConverter &) = delete;
    AudioConverter &operator=(const AudioConverter &) = delete;

    /**
     * @brief 配置目标音频格式
     * @param sampleRate 目标采样率
     * @param channels 目标声道数
     * @param format 目标样本格式，平面/交错布局由该格式决定
     * @return 是否配置成功
     */
    bool configure(int sampleRate, int channels, AudioSampleFormat format);

    /**
     * @brief 设置播放倍速
     * @param speed 播放倍速，必须为有限正数且不大于 64
     * @return 是否设置成功
     */
    bool setSpeed(double speed);

    /**
     * @brief 将输入帧转换为配置的目标格式
     * @param input 输入音频帧
     * @return 转换后的音频帧，失败时返回无效帧
     */
    Frame convert(const Frame &input);

    /**
     * @brief 转换为交错 PCM 并复用调用方缓冲区
     * @param input 输入音频帧
     * @param output 输出的交错 PCM 数据
     * @param sampleCount 每声道输出样本数
     * @return 是否转换成功
     */
    bool convert(const Frame &input, std::vector<uint8_t> &output, int &sampleCount);

    /**
     * @brief 清理转换上下文和缓存的重采样数据
     */
    void reset();

private:
    std::unique_ptr<internal::AudioConverter> impl_;
};

} // namespace decoder_sdk

#endif // DECODER_SDK_AUDIO_CONVERTER_H
