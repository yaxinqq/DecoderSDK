#include "VideoEncoder.h"

#include <iostream>

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder()
{
    close();
}

bool VideoEncoder::open(const std::string &outputUrl)
{
    if (opened_) {
        std::cerr << "Encoder is already opened" << std::endl;
        return false;
    }

    // 验证配置
    if (config_.url.empty()) {
        config_.url = outputUrl;
    }

    // 打开编码器
    if (!controller_.open(config_.url, config_)) {
        std::cerr << "Failed to open encoder for: " << config_.url << std::endl;
        return false;
    }

    opened_ = true;
    return true;
}

void VideoEncoder::close()
{
    if (!opened_) {
        return;
    }

    // 停止编码并关闭
    stop();
    controller_.close();
    opened_ = false;
}

void VideoEncoder::start()
{
    if (!opened_) {
        std::cerr << "Encoder is not opened" << std::endl;
        return;
    }

    controller_.start();
}

void VideoEncoder::stop()
{
    if (!opened_) {
        return;
    }

    controller_.stop();
}

decoder_sdk::Frame VideoEncoder::getWriteableVideoFrame()
{
    return controller_.getWriteableVideoFrame();
}

decoder_sdk::Frame VideoEncoder::getWriteableAudioFrame()
{
    return controller_.getWriteableAudioFrame();
}

bool VideoEncoder::pushVideoFrame(const decoder_sdk::Frame &frame)
{
    if (!opened_) {
        std::cerr << "Encoder is not opened" << std::endl;
        return false;
    }

    if (!frame.isValid()) {
        std::cerr << "Invalid frame provided to encoder" << std::endl;
        return false;
    }

    controller_.pushVideoFrame(frame);
    return true;
}

bool VideoEncoder::pushAudioFrame(const decoder_sdk::Frame &frame)
{
    if (!opened_) {
        std::cerr << "Encoder is not opened" << std::endl;
        return false;
    }

    if (!frame.isValid()) {
        std::cerr << "Invalid audio frame provided to encoder" << std::endl;
        return false;
    }

    controller_.pushAudioFrame(frame);
    return true;
}

uint64_t VideoEncoder::getEncodedBytes() const
{
    // 获取编码的统计信息（如果可用）
    // 这是一个占位符实现，实际值需要从编码器获取
    return 0;
}
