#include "zohe_ws_decoder_controller.h"

#include "logger/logger.h"
#include "utils/common_utils.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace {
// 硬解出错，降级到软解的容忍时间
constexpr int kDefaultFallbackToleranceTime = 1000; // 单位：毫秒
}

ZoheWsDecoderController::ZoheWsDecoderController(const Config &config) : config_(config)
{
	// 查找当前可用的硬解码器类型
    HardwareAccel::getSupportedHWAccelTypes();
}

ZoheWsDecoderController::~ZoheWsDecoderController()
{
    cleanup();
}

bool ZoheWsDecoderController::initDecoder(AVCodecID codecId, int width, int height,
                                          const uint8_t *extraData, int extraDataSize)
{
    if (initialized_) {
        cleanup();
    }

    // 查找解码器
    const AVCodec *codec = avcodec_find_decoder(codecId);
    if (!codec) {
        LOG_ERROR("Decoder not found for codec ID: {}", static_cast<int>(codecId));
        return false;
    }

    // 分配解码器上下文
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        LOG_ERROR("Failed to allocate decoder context");
        return false;
    }

    // 记录codecId
    codecId_ = codecId;
    // 设置基本参数
    codecCtx_->width = width;
    codecCtx_->height = height;
    codecCtx_->pix_fmt = AV_PIX_FMT_YUV420P;

    // 设置extradata
    if (extraData && extraDataSize > 0) {
        codecCtx_->extradata =
            static_cast<uint8_t *>(av_mallocz(extraDataSize + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!codecCtx_->extradata) {
            LOG_ERROR("Failed to allocate extradata");
            cleanup();
            return false;
        }
        memcpy(codecCtx_->extradata, extraData, extraDataSize);
        codecCtx_->extradata_size = extraDataSize;
    }

    // 设置硬件加速
    if (enableHardwareAccel_ && setupHardwareAcceleration()) {
        LOG_INFO("Hardware acceleration enabled: {}", hwAccel_->getDeviceName());
    } else {
        LOG_INFO("Using software decoding");
    }

    // 打开解码器
    int ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to open decoder: {}", errBuf);
        cleanup();
        return false;
    }

    // 分配帧
    frame_ = av_frame_alloc();
    if (!frame_) {
        LOG_ERROR("Failed to allocate frame");
        cleanup();
        return false;
    }

    // 分配数据包
    packet_ = av_packet_alloc();
    if (!packet_) {
        LOG_ERROR("Failed to allocate packet");
        cleanup();
        return false;
    }

    initialized_ = true;
    LOG_INFO("Decoder initialized successfully for codec: {}", codec->name);
    return true;
}

void ZoheWsDecoderController::setFrameCallback(
    std::function<void(const decoder_sdk::Frame &frame)> callback)
{
    frameCallback_ = std::move(callback);
}

bool ZoheWsDecoderController::pushPacket(const uint8_t *data, int size, int64_t pts, int64_t dts,
                                         MediaType mediaType)
{
    if (!initialized_ || !data || size <= 0) {
        return false;
    }

    // 设置数据包
    packet_->data = const_cast<uint8_t *>(data);
    packet_->size = size;
    packet_->pts = pts;
    packet_->dts = pts;

    // 发送数据包到解码器
    int ret = avcodec_send_packet(codecCtx_, packet_);
    if (ret != 0) {
        // 如果是EAGAIN或EOF错误，继续等待下一个包
        if (ret == AVERROR(EAGAIN) || ret == AVERROR(EOF)) {
            return false;
        }

        // 是否需要退化到软解
        const auto shouldFallback = !readFirstFrame_ && shouldFallbackToSoftware(ret);

        // 记录出错的信息
        LOG_WARN("Failed to send packet to decoder, error code: {}, error string: {}", ret,
                 utils::avErr2Str(ret));

        // 记录出错时间
        const auto currentTime = std::chrono::system_clock::now();
        if (!errorStartTime_.has_value()) {
            errorStartTime_ = currentTime; // 记录第一次错误时间
        }

        // 判断是否超过容忍时间
        if (std::chrono::duration_cast<std::chrono::milliseconds>(currentTime -
                                                                  errorStartTime_.value())
                .count() < kDefaultFallbackToleranceTime) {
            return false; // 未超过容忍时间，继续等待
        }

        // 如果超过容忍时间，尝试软解
        if (reinitializeWithSoftwareDecoder()) {
            LOG_INFO("Video Decoder: Fallback to software decoding.");
            errorStartTime_.reset(); // 重置错误计时
            readFirstFrame_ = false;

            // 重新送包
            return pushPacket(data, size, pts, dts, mediaType);
        } else {
            LOG_ERROR("Video Decoder: Failed to reinitialize with software decoder.");

            errorStartTime_.reset(); // 重置错误计时
            readFirstFrame_ = false;
        }
        // 等待下一个包
        return false;
    }

    // 接收解码后的帧
    while (ret >= 0) {
        ret = avcodec_receive_frame(codecCtx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            char errBuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errBuf, sizeof(errBuf));
            LOG_ERROR("Failed to receive frame from decoder: {}", errBuf);
            break;
        }

        readFirstFrame_ = true;
        errorStartTime_.reset();

        // 处理解码后的帧
        processDecodedFrame(mediaType);
        av_frame_unref(frame_);
    }

    // 重置数据包
    av_packet_unref(packet_);
    return true;
}

void ZoheWsDecoderController::flush(MediaType mediaType)
{
    if (!initialized_) {
        return;
    }

    // 发送空数据包以刷新解码器
    int ret = avcodec_send_packet(codecCtx_, nullptr);
    if (ret < 0) {
        return;
    }

    // 接收剩余的帧
    while (ret >= 0) {
        ret = avcodec_receive_frame(codecCtx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            break;
        }

        processDecodedFrame(mediaType);
        av_frame_unref(frame_);
    }
}

void ZoheWsDecoderController::cleanup()
{
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }

    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }

    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }

    if (codecCtx_) {
        if (codecCtx_->extradata) {
            av_freep(&codecCtx_->extradata);
            codecCtx_->extradata_size = 0;
        }

        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
    }

    codecId_ = AV_CODEC_ID_NONE;
    hwAccel_.reset();
    swFrame_ = internal::Frame();
    memoryFrame_ = internal::Frame();
    initialized_ = false;

    readFirstFrame_ = false;
    errorStartTime_.reset();
}

bool ZoheWsDecoderController::isInitialized() const
{
    return initialized_;
}

bool ZoheWsDecoderController::setupHardwareAcceleration()
{
    // 使用硬件加速工厂创建硬件加速器
    hwAccel_ = internal::HardwareAccelFactory::getInstance().createHardwareAccel(
        config_.hwAccelType, config_.hwDeviceIndex, config_.createHwContextCallback);

    if (!hwAccel_ || !hwAccel_->isInitialized()) {
        LOG_WARN("Hardware acceleration not available");
        return false;
    }

    // 设置解码器使用硬件加速
    if (!hwAccel_->setupDecoder(codecCtx_)) {
        LOG_WARN("Failed to setup hardware acceleration for decoder");
        hwAccel_.reset();
        return false;
    }

    return true;
}

void ZoheWsDecoderController::processDecodedFrame(MediaType mediaType)
{
    if (!frame_ || !frameCallback_) {
        return;
    }

    Frame resultFrame(frame_);
    // 设置媒体类型
    resultFrame.setMediaType(utils::mediaType2AVMediaType(mediaType));

    const bool isHardwareFrame = hwAccel_ && frame_->hw_frames_ctx;
    const auto swAVFormat = utils::imageFormat2AVPixelFormat(config_.swVideoOutFormat);
    bool needToConvert = false;

    // 硬件帧处理
    if (isHardwareFrame) {
        if (config_.requireFrameInSystemMemory) {
            resultFrame = transferHardwareFrame();
            resultFrame.setMediaType(utils::mediaType2AVMediaType(mediaType));

            needToConvert = resultFrame.pixelFormat() != swAVFormat;
        }
    } else {
        // 软件帧处理
        needToConvert = resultFrame.pixelFormat() != swAVFormat;
    }

    // 检查是否还需要格式转换
    if (needToConvert) {
        resultFrame = convertSoftwareFrame(resultFrame);
    }

    if (!resultFrame.isValid()) {
        LOG_ERROR("Failed to process decoded frame");
        return;
    }

    // 创建公共API的Frame对象
    auto publicFrame = std::make_unique<Frame>(std::move(resultFrame));
    decoder_sdk::Frame frame(std::move(publicFrame));

    // 调用回调函数
    frameCallback_(frame);
}

Frame ZoheWsDecoderController::transferHardwareFrame()
{
    if (!memoryFrame_.isValid()) {
        memoryFrame_.ensureAllocated();
    }

    if (!hwAccel_->transferFrameToHost(frame_, memoryFrame_.get())) {
        LOG_ERROR("Failed to transfer hardware frame to host memory");
        return internal::Frame();
    }

    // 创建新Frame并移动，避免拷贝
    internal::Frame result = std::move(memoryFrame_);
    memoryFrame_ = internal::Frame(); // 重置为空，下次会重新分配
    return result;
}

Frame ZoheWsDecoderController::convertSoftwareFrame(const Frame &frame)
{
    if (!frame.isValid())
        return {};

    if (!swFrame_.isValid()) {
        swFrame_.ensureAllocated();
    }

    // 初始化转换上下文
    const auto avFormat = utils::imageFormat2AVPixelFormat(config_.swVideoOutFormat);

    swsCtx_ = sws_getCachedContext(swsCtx_, frame.width(), frame.height(), frame.pixelFormat(),
                                   frame.width(), frame.height(), avFormat, SWS_BILINEAR, nullptr,
                                   nullptr, nullptr);

    if (!swsCtx_) {
        LOG_ERROR("Failed to create SwsContext");
        return internal::Frame();
    }

    // 设置目标帧参数
    swFrame_.setPixelFormat(avFormat);
    swFrame_.setWidth(frame.width());
    swFrame_.setHeight(frame.height());
    swFrame_.setAvPts(frame.avPts());
    swFrame_.setMediaType(frame.mediaType());

    // 分配缓冲区
    int ret = av_frame_get_buffer(swFrame_.get(), 0);
    if (ret < 0) {
        LOG_ERROR("Failed to allocate frame buffer");
        return internal::Frame();
    }

    // 执行转换
    ret = sws_scale(swsCtx_, (const uint8_t *const *)frame.get()->data, frame.get()->linesize, 0,
                    frame.height(), swFrame_.get()->data, swFrame_.get()->linesize);

    if (ret <= 0) {
        LOG_ERROR("Failed to scale frame");
        return internal::Frame();
    }

    // 复制帧属性
    av_frame_copy_props(swFrame_.get(), frame.get());

    // 创建新Frame并移动，避免拷贝
    internal::Frame result = std::move(swFrame_);
    swFrame_ = internal::Frame(); // 重置为空，下次会重新分配
    return result;
}

bool ZoheWsDecoderController::reinitializeWithSoftwareDecoder()
{
    LOG_INFO("Attempting to reinitialize decoder with software decoding");

    // 重置初始化完成标志
    initialized_ = false;

    // 记录当前参数
    const int width = codecCtx_->width;
    const int height = codecCtx_->height;
    const AVPixelFormat format =  codecCtx_->pix_fmt;

    const int extraDataSize = codecCtx_->extradata_size;
    uint8_t *extraData = static_cast<uint8_t *>(av_mallocz(extraDataSize + AV_INPUT_BUFFER_PADDING_SIZE));
    memcpy(extraData, codecCtx_->extradata, extraDataSize);

    // 关闭当前解码器
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
    }

    // 重置硬件加速器
    hwAccel_.reset();

    // 查找解码器（强制使用软件解码器）
    const AVCodec *codec = avcodec_find_decoder(codecId_);
    if (!codec) {
        LOG_ERROR("Software decoder not found for codec {}", static_cast<int>(codecId_));
        av_freep(&extraData);
        return false;
    }

    // 分配解码器上下文
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        LOG_ERROR("Failed to allocate software decoder context");
        av_freep(&extraData);
        return false;
    }

    // 设置基本参数
    codecCtx_->width = width;
    codecCtx_->height = height;
    codecCtx_->pix_fmt = format;

    // 设置extradata
    if (extraData && extraDataSize > 0) {
        codecCtx_->extradata =
            static_cast<uint8_t *>(av_mallocz(extraDataSize + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!codecCtx_->extradata) {
            LOG_ERROR("Failed to allocate extradata");
            cleanup();
            av_freep(&extraData);
            return false;
        }
        memcpy(codecCtx_->extradata, extraData, extraDataSize);
        codecCtx_->extradata_size = extraDataSize;
    }
    // 释放extraData
    av_freep(&extraData);

    // 打开软件解码器（不设置硬件加速）
    const int ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to open software decoder: {}", errBuf);
        cleanup();
        return false;
    }

    LOG_INFO("Successfully switched to software decoding for codec: {}", codec->name);

    // 刷新解码器缓冲区
    avcodec_flush_buffers(codecCtx_);

    // 初始化完成
    initialized_ = true;

    return true;
}

bool ZoheWsDecoderController::shouldFallbackToSoftware(int errorCode) const
{
    // 检查退化条件：
    // 1. 启用了硬件解码退化功能
    // 2. 当前使用硬件解码（有硬件设备上下文）
    // 3. 错误码是 AVERROR_INVALIDDATA
    // 4. 还未解码出过视频帧
    // 5. 硬件解码还未失败过（避免重复尝试）
    return config_.enableHardwareFallback && codecCtx_ && codecCtx_->hw_device_ctx &&
           (errorCode == AVERROR_INVALIDDATA || errorCode == -1);
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END