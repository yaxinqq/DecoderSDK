#include "VideoDecoder.h"

#include <iostream>

VideoDecoder::VideoDecoder()
{
    // 注册日志
    registerEventLogger();
}

VideoDecoder::~VideoDecoder()
{
    // 清理
    close();
}

bool VideoDecoder::open(const std::string &url_or_path)
{
    if (opened_.load()) {
        return true;
    }

    // 进行解码器配置
    decoder_sdk::DecoderConfig cfg;
    cfg.decodeMediaTypes = decoder_sdk::MediaType::kVideo;
    cfg.hwAccelType = decoder_sdk::HWAccelType::kCuda;
    cfg.requireFrameInSystemMemory = false;
    cfg.swVideoOutFormat = decoder_sdk::ImageFormat::kRGBA;
    cfg.enableFrameRateControl = true;

    // 初始化cuda上下文
    if (!cuda_.initialize(cfg.hwDeviceIndex)) {
        std::cout << "[CUDA] Failed to initialize CUDA context" << std::endl;
        return false;
    }

    // 配置解码器硬件上下文，保证解码渲染一致
    cfg.createHwContextCallback = [this](decoder_sdk::HWAccelType type) -> void * {
        if (type == decoder_sdk::HWAccelType::kCuda) {
            return reinterpret_cast<void *>(cuda_.context());
        }
        return nullptr;
    };
    cfg.freeHwContextCallback = [this](decoder_sdk::HWAccelType type, void *userHwContext) {
        (void)userHwContext;
        if (type == decoder_sdk::HWAccelType::kCuda) {
            cuda_.shutdown();
        }
    };

    // 验证是否打开成功
    const bool ok = controller_.open(url_or_path, cfg);
    if (!ok) {
        return false;
    }

    // 开始解码
    controller_.startDecode();
    opened_.store(true);
    return true;
}

void VideoDecoder::close()
{
    if (!opened_.load()) {
        return;
    }

    // 停止解码并关闭
    if (!controller_.isDecodeStopped()) {
        controller_.stopDecode();
    }
    controller_.close();
    opened_.store(false);
}

bool VideoDecoder::tryPopVideoFrame(decoder_sdk::Frame &outFrame, int timeoutMs)
{
    // 尝试从视频帧队列中取出一帧
    if (!opened_.load()) {
        return false;
    }
    return controller_.videoQueue().pop(outFrame, timeoutMs);
}

void VideoDecoder::registerEventLogger()
{
    controller_.addGlobalEventListener(
        [](decoder_sdk::EventType type, std::shared_ptr<decoder_sdk::EventArgs> event) {
            if (!event) {
                return;
            }
            std::cout << "[DecoderSDK] " << decoder_sdk::getEventTypeName(type) << " | "
                      << event->description << " | err=" << event->errorCode
                      << " | msg=" << event->errorMessage << std::endl;
        });
}
