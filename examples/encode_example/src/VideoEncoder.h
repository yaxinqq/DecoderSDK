#pragma once

#include "decodersdk/encoder_controller.h"
#include "decodersdk/encoder_define.h"
#include "decodersdk/frame.h"

#include <string>

/**
 * @brief DecoderSDK 编码器封装（视频编码）。
 */
class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    /**
     * @brief 设置编码配置
     *
     * @param config 编码配置
     */
    void setConfig(const decoder_sdk::EncoderConfig &config) { config_ = config; }

    /**
     * @brief 打开编码器
     *
     * @param outputUrl 输出地址（文件路径或URL）
     * @return 是否打开成功
     */
    bool open(const std::string &outputUrl);

    /**
     * @brief 关闭编码器
     */
    void close();

    /**
     * @brief 启动编码器
     */
    void start();

    /**
     * @brief 停止编码器
     */
    void stop();

    /**
     * @brief 获得可写的视频帧
     * @return 可写的视频帧，使用前需检查isValid
     */
    decoder_sdk::Frame getWriteableVideoFrame();

    /**
     * @brief 获得可写的音频帧
     * @return 可写的音频帧，使用前需检查isValid
     */
    decoder_sdk::Frame getWriteableAudioFrame();

    /**
     * @brief 推送视频帧进行编码
     *
     * @param frame 视频帧
     * @return 是否推送成功
     */
    bool pushVideoFrame(const decoder_sdk::Frame &frame);

    /**
     * @brief 推送音频帧进行编码
     *
     * @param frame 音频帧
     * @return 是否推送成功
     */
    bool pushAudioFrame(const decoder_sdk::Frame &frame);

    /**
     * @brief 获取编码器的写入位置信息
     *
     * @return 文件大小或字节数
     */
    uint64_t getEncodedBytes() const;

private:
    // 编码器
    decoder_sdk::EncoderController controller_;
    // 编码器配置
    decoder_sdk::EncoderConfig config_;
    // 是否已打开
    bool opened_ = false;
};
