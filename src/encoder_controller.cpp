#include "encoder_controller.h"
#include "encoder/audio_encoder.h"
#include "encoder/video_encoder.h"
#include "logger/logger.h"
#include "muxer/muxer.h"
#include "utils/common_utils.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace {
constexpr char kModuleName[] = "EncoderController";
}

EncoderController::EncoderController()
{
    muxer_ = std::make_shared<Muxer>();
    LOG_INFO("EncoderController initialized");
}

EncoderController::~EncoderController()
{
    close();
}

bool EncoderController::open(const std::string &url, const EncoderConfig &config)
{
    LOG_INFO("Opening encoder: {}", url);
    std::lock_guard<std::mutex> lock(mutex_);

    if (isOpened_) {
        LOG_WARN("Encoder already opened");
        return true;
    }

    // 打开流程：Muxer -> 添加视频/音频流 -> 写文件头
    bool ret = openInternal(url, config);
    if (ret) {
        LOG_INFO("Successfully opened encoder: {}", url);
    } else {
        LOG_ERROR("Failed to open encoder: {}", url);
    }
    return ret;
}

void EncoderController::close()
{
    LOG_INFO("Closing encoder controller");
    stop();

    std::lock_guard<std::mutex> lock(mutex_);
    closeInternal();
    LOG_INFO("Encoder controller closed");
}

void EncoderController::start()
{
    LOG_INFO("Starting encoder");
    std::lock_guard<std::mutex> lock(mutex_);

    if (!isOpened_) {
        LOG_ERROR("Cannot start: encoder not opened");
        return;
    }

    if (isStarted_)
        return;

    // 启动线程化编码循环（视频/音频分别一条线程）
    if (videoEncoder_) {
        videoEncoder_->start();
        LOG_DEBUG("Video encoder started");
    }
    if (audioEncoder_) {
        audioEncoder_->start();
        LOG_DEBUG("Audio encoder started");
    }

    isStarted_ = true;
    LOG_INFO("Encoder started successfully");
}

void EncoderController::stop()
{
    LOG_INFO("Stopping encoder");
    std::lock_guard<std::mutex> lock(mutex_);

    if (!isStarted_)
        return;

    if (videoEncoder_) {
        videoEncoder_->stop();
        LOG_DEBUG("Video encoder stopped");
    }
    if (audioEncoder_) {
        audioEncoder_->stop();
        LOG_DEBUG("Audio encoder stopped");
    }

    isStarted_ = false;
    LOG_INFO("Encoder stopped successfully");
}

bool EncoderController::getWriteableFrame(MediaType mediaType, Frame &frame) const
{
    switch (mediaType) {
        case MediaType::kVideo:
            return videoEncoder_->getWritableFrame(frame);
        case MediaType::kAudio:
            return audioEncoder_->getWritableFrame(frame);
        default:
            break;
    }

    LOG_ERROR("Unsupported media type: {}", static_cast<int>(mediaType));
    return false;
}

bool EncoderController::pushFrame(MediaType mediaType, const Frame &frame)
{
    if (!isOpened_ || !isStarted_)
        return false;

    switch (mediaType) {
        case MediaType::kVideo:
            return videoEncoder_->pushFrame(frame);
        case MediaType::kAudio:
            return audioEncoder_->pushFrame(frame);
        default:
            break;
    }

    LOG_ERROR("Unsupported media type: {}", static_cast<int>(mediaType));
    return false;
}

bool EncoderController::isOpened() const
{
    return isOpened_;
}

bool EncoderController::openInternal(const std::string &url, const EncoderConfig &config)
{
    url_ = url;
    config_ = config;

    if (!muxer_->open(url, config.format)) {
        LOG_ERROR("Failed to open muxer: {}", url);
        return false;
    }

    if (config.encodeMediaTypes.has(MediaType::kVideo)) {
        videoEncoder_ = std::make_shared<VideoEncoder>(muxer_);
        if (!videoEncoder_->open(config)) {
            LOG_ERROR("Failed to open video encoder");
            muxer_->close();
            return false;
        }
        LOG_DEBUG("Video encoder created");
    }

    if (config.encodeMediaTypes.has(MediaType::kAudio)) {
        audioEncoder_ = std::make_shared<AudioEncoder>(muxer_);
        if (!audioEncoder_->open(config)) {
            LOG_ERROR("Failed to open audio encoder");
            muxer_->close();
            return false;
        }
        LOG_DEBUG("Audio encoder created");
    }

    // Write header after adding streams
    if (!muxer_->writeHeader()) {
        LOG_ERROR("Failed to write header");
        muxer_->close();
        return false;
    }

    isOpened_ = true;
    return true;
}

void EncoderController::closeInternal()
{
    if (muxer_) {
        if (isOpened_) {
            muxer_->writeTrailer();
        }
        muxer_->close();
    }

    videoEncoder_.reset();
    audioEncoder_.reset();

    isOpened_ = false;
    isStarted_ = false;
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END
