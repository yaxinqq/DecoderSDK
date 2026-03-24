#ifndef DECODER_SDK_INTERNAL_ENCODER_CONTROLLER_H
#define DECODER_SDK_INTERNAL_ENCODER_CONTROLLER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "base/base_define.h"
#include "base/frame.h" // Internal Frame
#include "include/decodersdk/common_define.h"

// Forward declarations
DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN
class Muxer;
class VideoEncoder;
class AudioEncoder;
INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class EncoderController {
public:
    /**
     * @brief 构造函数
     */
    EncoderController();

    /**
     * @brief 析构函数
     */
    ~EncoderController();

    /**
     * @brief 打开编码器
     * @param url 输出URL
     * @param config 编码配置
     * @return 成功返回true
     *
     * 说明：
     * - 内部创建并打开 Muxer，随后根据配置创建并打开视频/音频编码器
     * - 写入容器文件头（writeHeader），确保后续可写包
     */
    bool open(const std::string &url, const EncoderConfig &config);

    /**
     * @brief 关闭编码器
     */
    void close();

    /**
     * @brief 启动编码
     */
    void start();

    /**
     * @brief 停止编码
     */
    void stop();

    /**
     * @brief 获得可写的视频帧
     * @param mediaType 媒体类型
     * @param frame 视频帧
     * @return 成功返回true
     */
    bool getWriteableFrame(MediaType mediaType, Frame &frame) const;

    /**
     * @brief 推送视频帧
     * @param mediaType 媒体类型
     * @param frame 视频帧
     * @return 成功返回true
     *
     * 线程安全说明：
     * - 在 open/start 后调用；内部做基本状态检查，不持有长期锁
     */
    bool pushFrame(MediaType mediaType, const Frame &frame);

    /**
     * @brief 检查是否已打开
     */
    bool isOpened() const;

private:
    /**
     * @brief 内部打开实现
     */
    bool openInternal(const std::string &url, const EncoderConfig &config);

    /**
     * @brief 内部关闭实现
     */
    void closeInternal();

private:
    std::shared_ptr<Muxer> muxer_;
    std::shared_ptr<VideoEncoder> videoEncoder_;
    std::shared_ptr<AudioEncoder> audioEncoder_;

    std::string url_;
    EncoderConfig config_;

    std::atomic<bool> isOpened_{false};
    std::atomic<bool> isStarted_{false};

    mutable std::mutex mutex_; // 保护内部状态
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_ENCODER_CONTROLLER_H
