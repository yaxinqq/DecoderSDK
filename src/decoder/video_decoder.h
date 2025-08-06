#ifndef DECODER_SDK_INTERNAL_VIDEO_DECODER_H
#define DECODER_SDK_INTERNAL_VIDEO_DECODER_H
#include <memory>
#include <optional>

#include "decoder_base.h"
#include "hardware_accel.h"

extern "C" {
#include <libswscale/swscale.h>
}

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class VideoDecoder : public DecoderBase {
public:
    /**
     * @brief 构造函数
     * @param demuxer 解复用器
     * @param StreamSyncManager 同步控制器
     * @param eventDispatcher 事件分发器
     */
    VideoDecoder(std::shared_ptr<Demuxer> demuxer,
                 std::shared_ptr<StreamSyncManager> StreamSyncManager,
                 std::shared_ptr<EventDispatcher> eventDispatcher);
    /**
     * @brief 析构函数
     */
    virtual ~VideoDecoder() override;

    /**
     * @brief 初始化视频解码器
     * @param config 配置参数项
     */
    void init(const Config &config);

    AVMediaType type() const override;

    /**
     * @brief 设置是否需要解码后的帧位于内存中
     * @param required 是否需要
     */
    void requireFrameInSystemMemory(bool required = true);

    // 获取检测到的帧率
    /**
     * @brief 获取检测到的帧率
     * @return 帧率
     */
    double getFrameRate() const;

protected:
    /**
     * @brief 解码循环
     */
    void decodeLoop() override;
    /**
     * @brief 硬件解码设置
     */
    bool setupHardwareDecode() override;
    /**
     * @brief 根据情况，是否清理解码器的硬件解码
     */
    bool removeHardwareDecode() override;

private:
    /**
     * @brief 处理帧格式转换
     * @param inputFrame 输入帧
     * @return 转换后的帧
     */
    Frame processFrameConversion(const Frame &inputFrame);

    /**
     * @brief 处理硬件帧到内存的转换
     * @param hwFrame 硬件帧
     * @return 转换后的帧
     */
    Frame transferHardwareFrame(const Frame &hwFrame);

    /**
     * @brief 处理软件帧格式转换
     * @param frame 软件帧
     * @return 转换后的帧
     */
    Frame convertSoftwareFrame(const Frame &frame);

    /**
     * @brief 检查是否应该退回到软件解码
     * @param errorCode 错误码
     * @param errorCode 错误码
     * @return 是否应该退回到软件解码
     */
    bool shouldFallbackToSoftware(int errorCode) const;

    /**
     * @brief 重新初始化软件解码器
     * @return 是否成功初始化
     */
    bool reinitializeWithSoftwareDecoder();

private:
    // 硬件加速器
    std::shared_ptr<HardwareAccel> hwAccel_;
    // 硬解加速类型
    HWAccelType hwAccelType_ = HWAccelType::kAuto;
    // 硬件设备ID
    int deviceIndex_ = 0;
    // 软解图像类型
    AVPixelFormat softPixelFormat_ = AV_PIX_FMT_YUV420P;
    // 是否需要在内存
    bool requireFrameInMemory_ = false;
    // 硬件上下文创建回调
    CreateHWContextCallback createHWContextCallback_ = nullptr;
    // 硬件上下文销毁回调
    FreeHWContextCallback freeHWContextCallback_ = nullptr;

    // 复用的转换上下文和帧
    SwsContext *swsCtx_ = nullptr;
    Frame memoryFrame_;
    Frame swsFrame_;

    // 硬件解码退化相关
    // 是否启用硬件解码退化
    bool enableHardwareFallback_ = true;

    // 是否需要手动修改SPS profile
    bool needFixSPSProfile_ = false;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_VIDEO_DECODER_H