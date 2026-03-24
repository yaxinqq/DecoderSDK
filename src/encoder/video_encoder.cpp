#include "video_encoder.h"
#include "decoder/hardware_accel.h"
#include "logger/logger.h"
#include "utils/common_utils.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace {
constexpr char kH264Prefix[] = "h264_";
constexpr char kHevcPrefix[] = "hevc_";

constexpr char kNvName[] = "nvenc";
constexpr char kAmfName[] = "amf";
constexpr char kQsvName[] = "qsv";
constexpr char kVappiName[] = "vaapi";

std::string codecPrefix(EncoderConfig::VideoCodec codec)
{
    switch (codec) {
        case EncoderConfig::VideoCodec::kH265:
            return kHevcPrefix;
        default:
            break;
    }

    return kH264Prefix;
}

AVCodecID swCodecId(EncoderConfig::VideoCodec codec)
{
    switch (codec) {
        case EncoderConfig::VideoCodec::kH265:
            return AV_CODEC_ID_HEVC;
        default:
            break;
    }

    return AV_CODEC_ID_H264;
}

const AVCodec *findByName(const std::string_view &name)
{
    if (name.empty()) {
        return nullptr;
    }

    return avcodec_find_encoder_by_name(name.data());
}

const AVCodec *findEncoder(EncoderConfig::VideoCodec videoCodec, HWAccelType hwAccelType)
{
    std::string prefix = codecPrefix(videoCodec);

    switch (hwAccelType) {
        case HWAccelType::kCuda:
            return findByName(prefix + kNvName);
        case HWAccelType::kAmf:
            return findByName(prefix + kAmfName);
        case HWAccelType::kQsv:
            return findByName(prefix + kQsvName);
        case HWAccelType::kVaapi:
            return findByName(prefix + kVappiName);
        default:
            break;
    }

    return nullptr;
}

} // namespace

VideoEncoder::VideoEncoder(std::shared_ptr<Muxer> muxer) : EncoderBase(muxer)
{
}

VideoEncoder::~VideoEncoder()
{
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    if (convertFrame_) {
        av_frame_free(&convertFrame_);
        convertFrame_ = nullptr;
    }
    if (transferFrame_) {
        av_frame_free(&transferFrame_);
        transferFrame_ = nullptr;
    }
    if (hwUploadFrame_) {
        av_frame_free(&hwUploadFrame_);
        hwUploadFrame_ = nullptr;
    }
}

bool VideoEncoder::open(const EncoderConfig &config)
{
    if (isOpened_)
        return true;
    config_ = config;

    // 初始化硬件加速
    hwAccel_.reset();
    const auto hwType = initHwAccelContext();

    const AVCodec *codec = findEncoder(config.videoCodec, hwType);
    if (!codec) {
        codec = avcodec_find_encoder(swCodecId(config.videoCodec));
    }
    if (!codec) {
        LOG_ERROR("Video codec not found");
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_)
        return false;

    codecCtx_->width = config.width;
    codecCtx_->height = config.height;
    codecCtx_->bit_rate = config.videoBitrate;
    codecCtx_->time_base = {1, config.fps};
    codecCtx_->pkt_timebase = {1, config.fps};
    codecCtx_->framerate = {config.fps, 1};
    codecCtx_->gop_size = config.gopSize;

    swPixFmt_ = utils::imageFormat2AVPixelFormat(config.encodeFormat);
    codecCtx_->pix_fmt = hwAccel_->getPixelFormat();
    codecCtx_->sw_pix_fmt = swPixFmt_;

    useHwFrames_ = false;
    if (hwAccel_ &&
        hwAccel_->setupEncoder(codecCtx_, swPixFmt_, codecCtx_->width, codecCtx_->height)) {
        useHwFrames_ = true;
    } else {
        LOG_WARN("Failed to setup encoder hw_frames_ctx, fallback to software path");
        hwAccel_.reset();

        codecCtx_->pix_fmt = swPixFmt_;
    }

    if (const auto foramtContext = muxer_->formatContext();
        foramtContext && (foramtContext->oformat->flags & AVFMT_GLOBALHEADER)) {
        codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    int ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        LOG_ERROR("Could not open video codec: {}", utils::avErr2Str(ret));
        return false;
    }

    streamIndex_ = muxer_->addStream(codecCtx_);
    if (streamIndex_ < 0) {
        LOG_ERROR("Could not add video stream");
        return false;
    }

    isOpened_ = true;
    LOG_INFO("Video encoder opened: {}x{}, {}fps, {}, hwFrames: {}", config.width, config.height,
             config.fps, codec->name, useHwFrames_);
    return true;
}

bool VideoEncoder::getWritableFrame(Frame &frame) const
{
    if (!codecCtx_)
        return false;

    // 确保帧有效
    frame.ensureAllocated();
    AVFrame *avFrame = frame.get();

    av_frame_make_writable(avFrame);
    int ret = 0;
    if (codecCtx_->hw_frames_ctx) {
        // 获得硬件帧
        ret = av_hwframe_get_buffer(codecCtx_->hw_frames_ctx, avFrame, 0);
    } else {
        // 获得软件帧
        avFrame->format = codecCtx_->pix_fmt;
        avFrame->width = codecCtx_->width;
        avFrame->height = codecCtx_->height;

        ret = av_frame_get_buffer(avFrame, 0);
    }

    if (ret < 0) {
        LOG_WARN("Get frame buffer failed! Error code: {}, msg: {}", ret, utils::avErr2Str(ret));
        return false;
    }
    return true;
}

void VideoEncoder::encodeLoop()
{
    Frame frame;
    pts_ = 0;

    while (isRunning_) {
        if (frameQueue_->pop(frame, 100)) { // 100ms timeout
            if (!frame.isValid())
                continue;

            AVFrame *srcAVFrame = frame.get();
            const bool srcInHw = frame.isInHardware();
            const bool encInHw = useHwFrames_ && codecCtx_->hw_frames_ctx;
            const int64_t pts = pts_++;

            // 路径选择：
            // - srcInHw && encInHw：尽量直通或同设备迁移，不行则 hw->sw->swscale->hw
            // - srcInHw && !encInHw：下载为软帧，再按需 swscale
            // - !srcInHw && encInHw：软帧按需 swscale 后上传到编码器的硬件上下文
            // - !srcInHw && !encInHw：软帧按需 swscale 后直接编码
            if (srcInHw && encInHw) {
                const bool sameHwFormat = (srcAVFrame->format == codecCtx_->pix_fmt);
                const bool sameSize = (srcAVFrame->width == codecCtx_->width &&
                                       srcAVFrame->height == codecCtx_->height);
                const bool sameFramesCtx =
                    (srcAVFrame->hw_frames_ctx && codecCtx_->hw_frames_ctx &&
                     srcAVFrame->hw_frames_ctx->buffer == codecCtx_->hw_frames_ctx->buffer);

                if (sameHwFormat && sameSize && sameFramesCtx) {
                    // 硬件直通：完全一致时直接送入编码器
                    srcAVFrame->pts = pts;
                    encodeAndDrain(srcAVFrame);
                    releaseTemporaryFrames();
                    statistics_.framesEncoded++;
                    continue;
                }

                // 先尝试在编码器设备上下文内申请帧，然后做 hw->hw 迁移
                AVFrame *outHwFrame = allocEncoderHwFrame();
                if (outHwFrame) {
                    const int ret = av_hwframe_transfer_data(outHwFrame, srcAVFrame, 0);
                    if (ret >= 0) {
                        av_frame_copy_props(outHwFrame, srcAVFrame);
                        outHwFrame->pts = pts;
                        encodeAndDrain(outHwFrame);
                        releaseTemporaryFrames();
                        statistics_.framesEncoded++;
                        continue;
                    }
                }

                // 回退：硬件帧下载为软帧，并进行像素格式/尺寸转换再上传
                AVFrame *swFrame = downloadHwFrameToSw(srcAVFrame);
                if (!swFrame) {
                    continue;
                }
                AVFrame *convertedSwFrame = convertSwFrameForEncoder(swFrame);
                if (!convertedSwFrame) {
                    continue;
                }
                convertedSwFrame->pts = pts;

                AVFrame *uploadedHwFrame = uploadSwFrameToEncoderHw(convertedSwFrame);
                if (!uploadedHwFrame) {
                    continue;
                }
                uploadedHwFrame->pts = pts;

                encodeAndDrain(uploadedHwFrame);
                releaseTemporaryFrames();
                statistics_.framesEncoded++;
                continue;
            }

            AVFrame *swFrame = srcAVFrame;
            if (srcInHw) {
                // 硬件解码 + 软编码：先下载为软帧
                swFrame = downloadHwFrameToSw(srcAVFrame);
                if (!swFrame) {
                    continue;
                }
            }

            // 统一：确保软帧达到编码器要求的像素格式与尺寸
            AVFrame *convertedSwFrame = convertSwFrameForEncoder(swFrame);
            if (!convertedSwFrame) {
                continue;
            }
            convertedSwFrame->pts = pts;

            AVFrame *encodeAVFrame = convertedSwFrame;
            if (encInHw) {
                // 软解 + 硬编：将软帧上传到编码器的硬件上下文
                encodeAVFrame = uploadSwFrameToEncoderHw(convertedSwFrame);
                if (!encodeAVFrame) {
                    continue;
                }
                encodeAVFrame->pts = pts;
            }

            encodeAndDrain(encodeAVFrame);
            releaseTemporaryFrames();
            statistics_.framesEncoded++;
        } else {
            // Check if aborted
            if (frameQueue_->size() == 0 && !isRunning_)
                break;
        }
    }

    // Flush encoder
    flush();
}

HWAccelType VideoEncoder::initHwAccelContext()
{
    // 创建硬件加速器（默认尝试自动选择最佳硬件加速方式）
    hwAccel_ = HardwareAccelFactory::getInstance().createHardwareAccel(
        config_.hwAccelType, config_.backendHwAccelType, config_.hwDeviceIndex,
        config_.createHwContextCallback, config_.freeHwContextCallback);
    if (!hwAccel_) {
        LOG_WARN("Hardware acceleration not available, using software decode");
        return HWAccelType::kNone;
    }

    return hwAccel_->getType();
}

void VideoEncoder::encodeAndDrain(AVFrame *frame)
{
    auto start = std::chrono::steady_clock::now();
    int ret = sendFrameToCodec(frame);
    if (ret < 0) {
        LOG_ERROR("Error sending frame to codec: {}", utils::avErr2Str(ret));
    }
    while (ret >= 0) {
        ret = receivePacketAndMux();
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            LOG_ERROR("Error receiving packet from codec: {}", utils::avErr2Str(ret));
            break;
        }
    }
    auto end = std::chrono::steady_clock::now();
    statistics_.totalEncodeTime +=
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

AVFrame *VideoEncoder::downloadHwFrameToSw(AVFrame *hwFrame)
{
    if (!transferFrame_) {
        transferFrame_ = av_frame_alloc();
    }
    if (!transferFrame_) {
        LOG_ERROR("Failed to allocate transfer frame");
        return nullptr;
    }
    av_frame_unref(transferFrame_);
    const int ret = av_hwframe_transfer_data(transferFrame_, hwFrame, 0);
    if (ret < 0) {
        LOG_ERROR("Failed to transfer hw frame to system memory: {}", utils::avErr2Str(ret));
        return nullptr;
    }
    av_frame_copy_props(transferFrame_, hwFrame);
    return transferFrame_;
}

AVFrame *VideoEncoder::convertSwFrameForEncoder(AVFrame *inSwFrame)
{
    const AVPixelFormat targetSwFmt = swPixFmt_;
    const int targetW = codecCtx_->width;
    const int targetH = codecCtx_->height;
    if (inSwFrame->format == targetSwFmt && inSwFrame->width == targetW &&
        inSwFrame->height == targetH) {
        return inSwFrame;
    }
    swsCtx_ = sws_getCachedContext(swsCtx_, inSwFrame->width, inSwFrame->height,
                                   (AVPixelFormat)inSwFrame->format, targetW, targetH, targetSwFmt,
                                   SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!swsCtx_) {
        LOG_ERROR("Could not initialize sws context");
        return nullptr;
    }
    if (!convertFrame_ || convertFrame_->format != targetSwFmt || convertFrame_->width != targetW ||
        convertFrame_->height != targetH) {
        if (convertFrame_) {
            av_frame_free(&convertFrame_);
        }
        convertFrame_ = av_frame_alloc();
        if (!convertFrame_) {
            LOG_ERROR("Failed to allocate convert frame");
            return nullptr;
        }
        convertFrame_->format = targetSwFmt;
        convertFrame_->width = targetW;
        convertFrame_->height = targetH;
        const int bufRet = av_frame_get_buffer(convertFrame_, 32);
        if (bufRet < 0) {
            LOG_ERROR("Failed to allocate convert frame buffer: {}", utils::avErr2Str(bufRet));
            av_frame_free(&convertFrame_);
            return nullptr;
        }
    }
    const int writableRet = av_frame_make_writable(convertFrame_);
    if (writableRet < 0) {
        LOG_ERROR("Failed to make convert frame writable: {}", utils::avErr2Str(writableRet));
        return nullptr;
    }
    const int scaleH = sws_scale(swsCtx_, inSwFrame->data, inSwFrame->linesize, 0,
                                 inSwFrame->height, convertFrame_->data, convertFrame_->linesize);
    if (scaleH <= 0) {
        LOG_ERROR("sws_scale failed");
        return nullptr;
    }
    av_frame_copy_props(convertFrame_, inSwFrame);
    return convertFrame_;
}

AVFrame *VideoEncoder::allocEncoderHwFrame()
{
    if (!hwUploadFrame_) {
        hwUploadFrame_ = av_frame_alloc();
    }
    if (!hwUploadFrame_) {
        LOG_ERROR("Failed to allocate hw upload frame");
        return nullptr;
    }
    av_frame_unref(hwUploadFrame_);
    const int ret = av_hwframe_get_buffer(codecCtx_->hw_frames_ctx, hwUploadFrame_, 0);
    if (ret < 0) {
        LOG_ERROR("Failed to get hw frame buffer: {}", utils::avErr2Str(ret));
        return nullptr;
    }
    return hwUploadFrame_;
}

AVFrame *VideoEncoder::uploadSwFrameToEncoderHw(AVFrame *inSwFrame)
{
    AVFrame *outHwFrame = allocEncoderHwFrame();
    if (!outHwFrame) {
        return nullptr;
    }
    const int ret = av_hwframe_transfer_data(outHwFrame, inSwFrame, 0);
    if (ret < 0) {
        LOG_ERROR("Failed to upload frame to hw: {}", utils::avErr2Str(ret));
        return nullptr;
    }
    av_frame_copy_props(outHwFrame, inSwFrame);
    return outHwFrame;
}

void VideoEncoder::releaseTemporaryFrames()
{
    if (transferFrame_)
        av_frame_unref(transferFrame_);
    if (convertFrame_)
        av_frame_unref(convertFrame_);
    if (hwUploadFrame_)
        av_frame_unref(hwUploadFrame_);
}
INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END
