/**
 * @file audio_converter.h
 * @brief 声明 DecoderSDK 内部使用的音频格式转换与重采样类。
 *
 * 该头文件属于内部实现层，仅供 SDK 内部和 src/api 转发层使用。
 * 对外公开接口位于 src/include/decodersdk/audio_converter.h。
 */

#ifndef DECODER_SDK_INTERNAL_AUDIO_CONVERTER_H
#define DECODER_SDK_INTERNAL_AUDIO_CONVERTER_H

#include "frame.h"

#include "include/decodersdk/common_define.h"

#include <cstdint>
#include <memory>
#include <vector>

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

/**
 * @brief 音频格式转换与重采样实现。
 *
 * 该类基于 FFmpeg 的 libswresample 完成以下工作：
 * - 将输入音频帧转换为指定的采样率、声道数和样本格式；
 * - 支持平面格式与交错格式之间的转换；
 * - 通过调整输入端的有效采样率实现播放倍速；
 * - 在输入参数发生变化时重新创建重采样上下文。
 *
 * 该类不支持拷贝，只允许移动。实例本身不提供线程安全保证，
 * 调用方需要确保同一实例不会被多个线程并发访问。
 */
class AudioConverter {
public:
    /**
     * @brief 创建音频转换器。
     *
     * 创建后尚未配置目标格式，必须调用 configure() 后才能执行转换。
     */
    AudioConverter();

    /**
     * @brief 销毁音频转换器并释放重采样上下文。
     */
    ~AudioConverter();

    /**
     * @brief 移动构造函数。
     *
     * 移动后源对象的内部实现会被转移，源对象不再持有有效转换上下文。
     */
    AudioConverter(AudioConverter &&other) noexcept;

    /**
     * @brief 移动赋值运算符。
     *
     * 当前对象原有的重采样上下文会被释放，然后接管源对象的实现。
     */
    AudioConverter &operator=(AudioConverter &&other) noexcept;

    /**
     * @brief 禁止拷贝构造。
     *
     * 转换器持有 FFmpeg 重采样上下文，不提供隐式资源共享语义。
     */
    AudioConverter(const AudioConverter &) = delete;

    /**
     * @brief 禁止拷贝赋值。
     */
    AudioConverter &operator=(const AudioConverter &) = delete;

    /**
     * @brief 配置目标音频格式。
     *
     * @param sampleRate 目标采样率，必须大于 0。
     * @param channels 目标声道数，必须大于 0。
     * @param format 目标样本格式，不支持未知格式和 64 位样本格式。
     * @return 配置成功返回 true，参数无效或格式不支持时返回 false。
     *
     * 如果目标格式与当前配置完全一致，则直接返回 true。
     * 如果目标格式发生变化，会释放现有重采样上下文，等待下一次转换时重建。
     */
    bool configure(int sampleRate, int channels, AudioSampleFormat format);

    /**
     * @brief 设置播放倍速。
     *
     * @param speed 播放倍速，必须是有限正数且不大于 64。
     * @return 设置成功返回 true，参数无效时返回 false。
     *
     * 倍速通过改变重采样输入端的有效采样率实现。倍速发生变化时，
     * 现有重采样上下文会失效并等待下一次转换时重建。
     */
    bool setSpeed(double speed);

    /**
     * @brief 将输入帧转换为配置好的目标音频格式。
     *
     * @param input 输入音频帧。
     * @return 转换成功时返回有效音频帧，失败时返回无效帧。
     *
     * 输出帧会继承输入帧的序列号、时间戳、媒体类型、SEI 数据等元数据。
     */
    Frame convert(const Frame &input);

    /**
     * @brief 将输入帧转换为交错 PCM，并复用调用方提供的缓冲区。
     *
     * @param input 输入音频帧。
     * @param output 输出的交错 PCM 数据，成功后会被调整为实际数据大小。
     * @param sampleCount 输出的每声道样本数。
     * @return 转换成功返回 true；输入无效、目标格式为平面格式或转换失败时返回 false。
     *
     * 该重载不支持输出平面格式。失败时 output 会被清空，sampleCount 会被置为 0。
     */
    bool convert(const Frame &input, std::vector<uint8_t> &output, int &sampleCount);

    /**
     * @brief 重置转换状态。
     *
     * 释放当前重采样上下文，并清除缓存的输入格式信息。
     * 目标采样率、声道数和样本格式配置会被保留，下一次转换时重新建立上下文。
     */
    void reset();

private:
    /**
     * @brief 隐藏 FFmpeg 类型和重采样状态的内部实现。
     */
    struct Impl;

    /**
     * @brief 转换器实现对象。
     */
    std::unique_ptr<Impl> impl_;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_AUDIO_CONVERTER_H
