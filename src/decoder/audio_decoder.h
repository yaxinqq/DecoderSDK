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
    virtual ~AudioDecoder();

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
     * @return 重采样后的音频帧
     */
    Frame resampleFrame(const Frame &frame);

    /**
     * @brief 检查是否需要重新初始化重采样
     * @param lastSpeed 上一次解码速度
     * @return 是否需要重新初始化
     */
    bool needResampleUpdate(double lastSpeed);

private:
    // 用于音频重采样
    SwrContext *swrCtx_ = nullptr;
    // 是否需要重采样
    bool needResample_ = false;

    // 复用的重采样帧
    Frame resampleFrame_;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_AUDIO_DECODER_H