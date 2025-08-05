#ifndef DECODER_SDK_INTERNAL_AUDIO_DECODER_H
#define DECODER_SDK_INTERNAL_AUDIO_DECODER_H
#include <chrono>
#include <optional>

extern "C" {
#include "libswresample/swresample.h"
}

#include "decoder_base.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class AudioDecoder : public DecoderBase {
public:
    /**
     * @brief 构造函数
     * @param demuxer 解复用器
     * @param StreamSyncManager 同步控制器
     * @param eventDispatcher 事件分发器
     */
    AudioDecoder(std::shared_ptr<Demuxer> demuxer,
                 std::shared_ptr<StreamSyncManager> StreamSyncManager,
                 std::shared_ptr<EventDispatcher> eventDispatcher);
    /**
     * @brief 析构函数
     */
    virtual ~AudioDecoder() override;

    /**
     * @brief 初始化音频解码器
     * @param config 配置参数项
     */
    void init(const Config &config);

    /**
     * @brief 获取音频解码器类型
     * @return 音频解码器类型
     */
    AVMediaType type() const override;

protected:
    /**
     * @brief 解码循环
     */
    virtual void decodeLoop() override;

private:
    /**
     * @brief 初始化重采样上下文
     * @return 是否成功初始化
     */
    bool initResampleContext();

    /**
     * @brief 重采样音频数据
     * @param frame 待重采样的音频帧
     * @param errorCode 错误码
     * @return 重采样后的音频帧
     */
    Frame resampleFrame(const Frame &frame, int &errorCode);

    /**
     * @brief 检查是否需要重新初始化重采样
     * @param lastSpeed 上一次解码速度
     * @return 是否需要重新初始化
     */
    bool needResampleUpdate(double lastSpeed);

    /**
     * @brief 初始化格式转换上下文
     * @param srcFormat 源格式
     * @param dstFormat 目标格式
     * @param sampleRate 采样率
     * @param channels 声道数
     * @param channelLayout 声道布局
     * @return 是否成功初始化
     */
    bool initFormatConvertContext(AVSampleFormat srcFormat, AVSampleFormat dstFormat,
                                  int sampleRate, int channels, uint64_t channelLayout);

    /**
     * @brief 使用格式转换上下文转换音频数据
     * @param frame 输入音频帧
     * @param targetFormat 目标格式
     * @return 是否转换成功
     */
    bool convertAudioFormat(Frame &frame, AVSampleFormat targetFormat);

    /**
     * @brief 清理重采样资源
     */
    void cleanupResampleResources();

    /**
     * @brief 清理格式转换资源
     */
    void cleanupFormatConvertResources();

private:
    // 用于音频重采样
    SwrContext *swrCtx_ = nullptr;
    // 是否需要重采样
    bool needResample_{false};

    // 复用的重采样帧
    Frame resampleFrame_;

    // 音频采样格式是否交错
    bool audioInterleaved_{true};

    // 用于格式转换的SwrContext（复用）
    SwrContext *formatConvertCtx_ = nullptr;
    // 格式转换的缓存参数，用于判断是否需要重新初始化
    AVSampleFormat lastSrcFormat_ = AV_SAMPLE_FMT_NONE;
    AVSampleFormat lastDstFormat_ = AV_SAMPLE_FMT_NONE;
    int lastConvertSampleRate_ = 0;
    int lastConvertChannels_ = 0;
    uint64_t lastConvertChannelLayout_ = 0;

    // 复用的格式转换帧
    Frame formatConvertFrame_;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_AUDIO_DECODER_H