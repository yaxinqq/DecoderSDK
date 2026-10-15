#ifndef DECODER_SDK_INTERNAL_AUDIO_DECODER_H
#define DECODER_SDK_INTERNAL_AUDIO_DECODER_H
#include <chrono>
#include <optional>

#include "decoder_base.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class AudioDecoder : public DecoderBase {
public:
    /**
     * @brief 构造函数
     * @param demuxer 解复用器
     * @param eventDispatcher 事件分发器
     * @param seekCoordinator Seek协调器
     * @param frameQueue 帧队列，由外部创建并管理生命周期
     */
    AudioDecoder(std::shared_ptr<Demuxer> demuxer,
                 std::shared_ptr<EventDispatcher> eventDispatcher,
                 std::shared_ptr<SeekCoordinator> seekCoordinator,
                 std::shared_ptr<FrameQueue> frameQueue);
    /**
     * @brief 析构函数
     */
    virtual ~AudioDecoder() override;

    /**
     * @brief 获取音频解码器类型
     * @return 音频解码器类型
     */
    AVMediaType type() const override;

    /**
     * @brief 获得解码器名称
     *
     * @return 解码器名称
     */
    const char *const decoderName() const override;

protected:
    /**
     * @brief 解码循环
     */
    virtual void decodeLoop() override;
    /**
     * @brief 组装解码器信息快照
     * @return 解码器信息
     */
    std::optional<DecoderInfo> buildDecoderInfo() const override;

};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_AUDIO_DECODER_H
