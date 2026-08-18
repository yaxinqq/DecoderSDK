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
     * @param eventDispatcher 事件分发器
     */
    VideoDecoder(std::shared_ptr<Demuxer> demuxer,
                 std::shared_ptr<EventDispatcher> eventDispatcher,
                 std::shared_ptr<SeekCoordinator> seekCoordinator);
    /**
     * @brief 析构函数
     */
    virtual ~VideoDecoder() override;

    /**
     * @brief 初始化视频解码器
     * @param config 配置参数项
     */
    void init(const DecoderConfig &config);

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

    /**
     * @brief 得到解码器信息
     *
     * @return 解码器信息
     */
    std::optional<DecoderInfo> decoderInfo() const override;

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
    void decodeLoop() override;
    /**
     * @brief 初始化硬件加速上下文，并返回硬件加速上下文类型
     * @return 硬件加速上下文类型
     */
    HWAccelType initHwAccelContext() override;
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

    /**
     * @brief 计算帧的持续时间 单位：s
     * @param frame 帧
     */
    double calculateFrameDuration(const Frame &frame, double defaultDuration) const;

    /*
     * @brief 处理关键帧错误
     * 当硬解解码到关键帧失败时，会调用此函数
     */
    void handleKeyFrameError(bool &hasKeyFrame, const std::string &errorString);

    /**
     * @brief 基于解码器实际像素格式和分辨率计算每像素比特数(bpp)
     *
     * 通过 av_image_get_buffer_size 获取缓冲区大小后换算，仅计算一次。
     */
    void calculateBitsPerPixel();

private:
    // 硬件加速器
    std::shared_ptr<HardwareAccel> hwAccel_;
    // 硬解加速类型
    HWAccelType hwAccelType_ = HWAccelType::kAuto;
    // 派生的硬件后端加速类型（仅QSV和AMF使用此参数，且仅Windows上生效）
    HWAccelType backendHwAccelType_ = HWAccelType::kD3d11va;
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
    // 是否解析用户自定义的SEI数据
    bool enableParseUserSEIData_ = false;

    // 复用的转换上下文和帧
    SwsContext *swsCtx_ = nullptr;
    Frame memoryFrame_;
    Frame swsFrame_;

    // 硬件解码退化相关
    // 是否启用硬件解码退化
    bool enableHardwareFallback_ = true;

    // 是否需要手动修改SPS profile
    bool needFixSPSProfile_ = false;

    // 基于 av_image_get_buffer_size 计算得到的每像素比特数，仅计算一次
    bool bppCalculated_ = false;
    int bitsPerPixel_ = 0;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_VIDEO_DECODER_H