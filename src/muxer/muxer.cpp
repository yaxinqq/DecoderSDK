#include "muxer.h"
#include "logger/logger.h"
#include "utils/common_utils.h"
#include <iostream>

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

Muxer::Muxer()
{
}

Muxer::~Muxer()
{
    close();
}

bool Muxer::open(const std::string &url, const std::string &format)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (isOpened_) {
        LOG_WARN("Muxer already opened");
        return true;
    }

    url_ = url;

    // 创建输出上下文：优先使用用户指定的容器格式，否则根据 URL 后缀猜测
    int ret = avformat_alloc_output_context2(
        &formatContext_, nullptr, format.empty() ? nullptr : format.c_str(), url.c_str());
    if (ret < 0 || !formatContext_) {
        // 若指定失败且格式为空，尝试自动猜测
        if (format.empty()) {
            ret = avformat_alloc_output_context2(&formatContext_, nullptr, nullptr, url.c_str());
        }
    }

    if (ret < 0 || !formatContext_) {
        LOG_ERROR("Could not create output context for {}, error: {}", url, utils::avErr2Str(ret));
        return false;
    }

    // 对于文件或需要显式 IO 的协议，打开 AVIO
    // 注意：某些协议（如 RTSP 推流）不需要（AVFMT_NOFILE）
    if (!(formatContext_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&formatContext_->pb, url.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOG_ERROR("Could not open output file {}, error: {}", url, utils::avErr2Str(ret));
            avformat_free_context(formatContext_);
            formatContext_ = nullptr;
            return false;
        }
    }

    isOpened_ = true;
    isHeaderWritten_ = false;
    LOG_INFO("Muxer opened successfully: {}", url);
    return true;
}

void Muxer::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!isOpened_)
        return;

    if (formatContext_) {
        if (isHeaderWritten_) {
            av_write_trailer(formatContext_);
        }

        if (!(formatContext_->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&formatContext_->pb);
        }
        avformat_free_context(formatContext_);
        formatContext_ = nullptr;
    }

    isOpened_ = false;
    isHeaderWritten_ = false;
    streams_.clear();
    LOG_INFO("Muxer closed");
}

int Muxer::addStream(AVCodecContext *codecCtx)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!formatContext_) {
        LOG_ERROR("Muxer not opened when adding stream");
        return -1;
    }

    AVStream *stream = avformat_new_stream(formatContext_, nullptr);
    if (!stream) {
        LOG_ERROR("Failed to allocate new stream");
        return -1;
    }

    // 拷贝编解码参数到容器流
    int ret = avcodec_parameters_from_context(stream->codecpar, codecCtx);
    if (ret < 0) {
        LOG_ERROR("Failed to copy codec parameters: {}", utils::avErr2Str(ret));
        return -1;
    }
    stream->codecpar->codec_tag = 0;

    // 明确设置时间基：避免 time_base 未被参数拷贝覆盖，可能在avformat_write_header时被容器调整
    // 初始设为编码器时间基，写包时再进行 rescale
    stream->time_base = codecCtx->time_base;

    stream->id = formatContext_->nb_streams - 1;
    stream->avg_frame_rate = codecCtx->framerate;
    streams_.push_back(stream);

    LOG_INFO("Added stream index: {}, codec: {}", stream->index,
             avcodec_get_name(codecCtx->codec_id));
    return stream->index;
}

bool Muxer::writeHeader()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!formatContext_ || isHeaderWritten_)
        return false;

    // 如需设置容器层选项，可通过 AVDictionary 传入
    AVDictionary *opt = nullptr;
    int ret = avformat_write_header(formatContext_, &opt);
    if (ret < 0) {
        LOG_ERROR("Error writing header: {}", utils::avErr2Str(ret));
        return false;
    }
    isHeaderWritten_ = true;
    LOG_INFO("Muxer header written");
    return true;
}

bool Muxer::writePacket(Packet &packet)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!formatContext_ || !isHeaderWritten_) {
        LOG_ERROR("Muxer not ready to write packet");
        return false;
    }

    AVPacket *pkt = packet.get();
    if (!pkt)
        return false;

    // 交错写入，容器内部完成时序处理
    int ret = av_interleaved_write_frame(formatContext_, pkt);
    if (ret < 0) {
        LOG_ERROR("Error writing packet: {}", utils::avErr2Str(ret));
        return false;
    }
    return true;
}

bool Muxer::writeTrailer()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!formatContext_ || !isHeaderWritten_)
        return false;

    int ret = av_write_trailer(formatContext_);
    if (ret < 0) {
        LOG_ERROR("Error writing trailer: {}", utils::avErr2Str(ret));
    }
    isHeaderWritten_ = false; // Prevent double write
    LOG_INFO("Muxer trailer written");
    return ret == 0;
}

bool Muxer::isOpened() const
{
    return isOpened_;
}

AVRational Muxer::getStreamTimeBase(int streamIndex)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (streamIndex >= 0 && streamIndex < (int)streams_.size()) {
        return streams_[streamIndex]->time_base;
    }
    return {1, 1000}; // Default or error
}

const AVFormatContext *const Muxer::formatContext() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return formatContext_;
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END
