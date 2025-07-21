#ifndef DECODER_SDK_INTERNAL_ZOHE_WEBSOCKET_DECODER_CONTROLLER_H
#define DECODER_SDK_INTERNAL_ZOHE_WEBSOCKET_DECODER_CONTROLLER_H
#include <functional>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include "base/base_define.h"
#include "base/frame.h"
#include "decoder/hardware_accel.h"
#include "include/decodersdk/common_define.h"
#include "include/decodersdk/frame.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

/**
 * @brief 自定义解码器控制器
 * 提供基于用户传入数据的解码功能，用于zohe的私有websocket协议
 */
class DECODER_SDK_API ZoheWsDecoderController {
public:
    /**
     * @brief 构造函数
     */
    explicit ZoheWsDecoderController(const Config &config);

    /**
     * @brief 析构函数
     */
    ~ZoheWsDecoderController();

    /**
     * @brief 初始化解码器
     * @param codecId 解码器ID (如 AV_CODEC_ID_H264, AV_CODEC_ID_H265)
     * @param width 视频宽度
     * @param height 视频高度
     * @param extraData 额外数据 (SPS/PPS等)
     * @param extraDataSize 额外数据大小
     * @return true 成功, false 失败
     */
    bool initDecoder(AVCodecID codecId, int width, int height, const uint8_t *extraData = nullptr,
                     int extraDataSize = 0);

    /**
     * @brief 设置帧回调函数
     * @param callback 回调函数
     */
    void setFrameCallback(std::function<void(const decoder_sdk::Frame &frame)> callback);

    /**
     * @brief 推送数据包进行解码
     * @param data 数据包数据
     * @param size 数据包大小
     * @param pts 展示时间戳 (可选)
     * @param dts 解码时间戳 (可选)
     * @return true 成功, false 失败
     */
    bool pushPacket(const uint8_t *data, int size, int64_t pts = AV_NOPTS_VALUE,
                    int64_t dts = AV_NOPTS_VALUE);

    /**
     * @brief 刷新解码器 (获取缓冲的帧)
     */
    void flush();

    /**
     * @brief 清理解码器
     */
    void cleanup();

    /**
     * @brief 检查解码器是否已初始化
     * @return true 已初始化, false 未初始化
     */
    bool isInitialized() const;

private:
    /**
     * @brief 初始化硬件加速
     * @return true 成功, false 失败
     */
    bool setupHardwareAcceleration();

    /**
     * @brief 处理解码后的帧
     */
    void processDecodedFrame();

    /**
     * @brief 转换硬件帧到内存帧
     * @return 转换后的内存帧
     */
    Frame transferHardwareFrame();

    /**
     * @brief 转换软件帧到内存帧
     * @return 转换后的内存帧
     */
    Frame convertSoftwareFrame(const Frame &frame);

private:
    Config config_;

    // FFmpeg相关
    AVCodecContext *codecCtx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVPacket *packet_ = nullptr;

    // 硬件加速
    std::shared_ptr<internal::HardwareAccel> hwAccel_;
    bool enableHardwareAccel_ = true;

    // 格式转换
    SwsContext *swsCtx_ = nullptr;
    Frame swFrame_;
    Frame memoryFrame_;

    // 回调函数
    std::function<void(const decoder_sdk::Frame &frame)> frameCallback_;

    // 状态
    bool initialized_ = false;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_ZOHE_WEBSOCKET_DECODER_CONTROLLER_H