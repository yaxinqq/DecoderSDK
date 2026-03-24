#include "VulkanOsdDemoApp.h"

#include "decodersdk/encoder_controller.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

VulkanOsdDemoApp::VulkanOsdDemoApp() = default;

VulkanOsdDemoApp::~VulkanOsdDemoApp() = default;

int VulkanOsdDemoApp::run(const std::string &inputPath, const std::string &outPath, bool debug)
{
    // 配置编码器，输出文件可按需修改，编码格式为简单考虑，这里固定为RGBA8，如果更改的话，需要对renderer进行适配
    decoder_sdk::EncoderConfig encoderConfig;
    encoderConfig.url = outPath;
    encoderConfig.width = 1280;
    encoderConfig.height = 720;
    encoderConfig.fps = 25;
    encoderConfig.hwAccelType = decoder_sdk::HWAccelType::kCuda;
    encoderConfig.encodeMediaTypes = decoder_sdk::MediaType::kVideo;
    encoderConfig.encodeFormat = decoder_sdk::ImageFormat::kRGBA;

    // 开启解码器
    if (!decoder_.open(inputPath)) {
        throw std::runtime_error("Failed to open decoder url: " + inputPath);
    }

    // 初始化渲染器
    if (!renderer_.initialize(encoderConfig.width, encoderConfig.height, VK_FORMAT_R8G8B8A8_UNORM,
                              debug)) {
        throw std::runtime_error("Failed to initialize Vulkan renderer");
    }

    // 设置解码器所使用的cuda上下文
    renderer_.setCudaContext(decoder_.cudaContext());

    // 初始化编码器
    decoder_sdk::EncoderController encoder;
    if (!encoder.open(encoderConfig.url, encoderConfig)) {
        throw std::runtime_error("Failed to open encoder");
    }
    encoder.start();

    // 循环取帧渲染
    while (!renderer_.shouldClose()) {
        // 处理事件
        renderer_.pollEvents();

        // 是否有待处理的帧
        bool hasPendingFrame = false;
        // 待处理的帧
        decoder_sdk::Frame pendingFrame;
        if (decoder_.tryPopVideoFrame(pendingFrame, 0) && pendingFrame.isValid() &&
            pendingFrame.pixelFormat() == decoder_sdk::ImageFormat::kCuda) {
            hasPendingFrame = true;
        }

        // 生成osd时间戳
        const std::string osd = formatCurrentTimestampUtf8();
        // 离屏渲染
        renderer_.drawFrame(pendingFrame, osd);

        // 如果待处理的帧有效，则进行编码
        if (hasPendingFrame) {
            decoder_sdk::Frame encodedFrame =
                encoder.getWriteableFrame(decoder_sdk::MediaType::kVideo);
            // 填充编码帧
            if (encodedFrame.isValid() && renderer_.fillEncodedFrame(encodedFrame)) {
                // 推送编码帧进行编码
                encoder.pushFrame(decoder_sdk::MediaType::kVideo, encodedFrame);
            }
        }
    }

    // 清理编码器和渲染器
    encoder.stop();
    encoder.close();
    decoder_.close();
    renderer_.shutdown();
    return 0;
}

std::string VulkanOsdDemoApp::formatCurrentTimestampUtf8()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);

    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    local = *std::localtime(&t);
#endif

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(4) << (local.tm_year + 1900) << "年";
    oss << std::setw(2) << (local.tm_mon + 1) << "月";
    oss << std::setw(2) << local.tm_mday << "日";
    oss << ' ';
    oss << std::setw(2) << local.tm_hour << ':';
    oss << std::setw(2) << local.tm_min << ':';
    oss << std::setw(2) << local.tm_sec;
    return oss.str();
}
