#ifndef DECODER_SDK_INTERNAL_VIDEO_ENCODER_H
#define DECODER_SDK_INTERNAL_VIDEO_ENCODER_H

#include "encoder_base.h"

extern "C" {
#include <libswscale/swscale.h>
}

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class HardwareAccel;

class VideoEncoder : public EncoderBase {
public:
    explicit VideoEncoder(std::shared_ptr<Muxer> muxer);
    ~VideoEncoder() override;

    MediaType mediaType() const override
    {
        return MediaType::kVideo;
    }

    bool open(const EncoderConfig &config) override;
    bool getWritableFrame(Frame &frame) const override;

protected:
    void encodeLoop() override;

private:
    /**
     * @brief 初始化硬件加速上下文，并返回硬件加速上下文类型
     * @return 硬件加速上下文类型
     */
    HWAccelType initHwAccelContext();

    /**
     * @brief 发送帧到编码器并拉取可用包写入复用器
     * @param frame 输入编码帧（软/硬）
     */
    void encodeAndDrain(AVFrame *frame);

    /**
     * @brief 下载硬件帧为系统内存帧
     * @param hwFrame 输入硬件帧
     * @return 成功返回软帧指针（可复用成员transferFrame_），失败返回nullptr
     */
    AVFrame *downloadHwFrameToSw(AVFrame *hwFrame);

    /**
     * @brief 将软帧转换到编码器要求的像素格式与尺寸
     * @param swFrame 输入软帧
     * @return 成功返回满足要求的软帧指针（可能为成员convertFrame_或原帧），失败返回nullptr
     */
    AVFrame *convertSwFrameForEncoder(AVFrame *swFrame);

    /**
     * @brief 在编码器的 hw_frames_ctx 中申请硬件帧缓冲
     * @return 成功返回硬件帧指针（成员hwUploadFrame_），失败返回nullptr
     */
    AVFrame *allocEncoderHwFrame();

    /**
     * @brief 将软帧上传到编码器硬件上下文
     * @param swFrame 输入软帧
     * @return 成功返回硬件帧指针（成员hwUploadFrame_），失败返回nullptr
     */
    AVFrame *uploadSwFrameToEncoderHw(AVFrame *swFrame);

    /**
     * @brief 释放（unref）一次编码迭代中产生的临时帧缓冲
     * 仅影响成员 transferFrame_ / convertFrame_ / hwUploadFrame_ 的数据引用，非释放对象本身
     */
    void releaseTemporaryFrames();

private:
    SwsContext *swsCtx_ = nullptr;
    AVFrame *convertFrame_ = nullptr;
    AVFrame *transferFrame_ = nullptr;
    AVFrame *hwUploadFrame_ = nullptr;
    std::shared_ptr<HardwareAccel> hwAccel_;
    bool useHwFrames_ = false;
    AVPixelFormat swPixFmt_ = AV_PIX_FMT_RGBA;
    int64_t pts_ = 0;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_VIDEO_ENCODER_H
