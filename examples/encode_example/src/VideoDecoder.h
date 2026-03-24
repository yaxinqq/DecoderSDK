#pragma once

#include "decodersdk/decoder_controller.h"
#include "decodersdk/frame.h"

#include <atomic>
#include <string>

#include "CudaContext.h"

/**
 * @brief DecoderSDK 解码封装（同步打开 + 主循环取帧）。
 */
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    /**
     * @brief 打开媒体。
     */
    bool open(const std::string &url_or_path);

    /**
     * @brief 关闭媒体。
     */
    void close();

    /**
     * @brief 尝试从解码队列取出一帧视频。
     * @param outFrame 输出帧。
     * @param timeoutMs 超时（毫秒）。
     * @return true 表示成功取到一帧；false 表示超时或队列为空。
     */
    bool tryPopVideoFrame(decoder_sdk::Frame &outFrame, int timeoutMs);

    /**
     * @brief 得到使用的CUDA上下文
     * 
     * @return cuda上下文
     */
    CUcontext cudaContext() const { return cuda_.context(); }

private:
    /**
     * @brief 注册事件日志
     * 
     */
    void registerEventLogger();

private:
    // 解码器
    decoder_sdk::DecoderController controller_;
    // 是否已打开
    std::atomic_bool opened_{false};
    // cuda上下文相关
    CudaContext cuda_;
};
