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
    virtual ~VideoDecoder();

    /**
     * @brief 初始化视频解码器
     * @param type 硬件加速类型
     * @param deviceIndex 设备索引
     * @param softPixelFormat 软件像素格式
     * @param requireFrameInMemory 是否需要将解码后的帧存储在内存中
     */
    void init(HWAccelType type = HWAccelType::kAuto, int deviceIndex = 0,
              AVPixelFormat softPixelFormat = AV_PIX_FMT_YUV420P,
              bool requireFrameInMemory = false);

    /**
     * @brief 打开视频解码器
     * @return 是否成功打开
     */
    bool open() override;
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

private:
    /**
     * @brief 更新视频帧率
     * @param frameRate 帧率
     */
    void updateFrameRate(AVRational frameRate);

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

private:
    // 检测到的帧率
    double frameRate_;

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

    // 复用的转换上下文和帧
    SwsContext *swsCtx_ = nullptr;
    Frame memoryFrame_;
    Frame swsFrame_;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_VIDEO_DECODER_H