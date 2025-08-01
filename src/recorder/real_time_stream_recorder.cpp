#include "real_time_stream_recorder.h"

extern "C" {
#include <libavutil/avutil.h>
}

#include "logger/logger.h"
#include "utils/common_utils.h"
#include <algorithm>
#include <filesystem>

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

RealTimeStreamRecorder::RealTimeStreamRecorder(std::shared_ptr<EventDispatcher> eventDispatcher)
    : eventDispatcher_(eventDispatcher)
{
    // 初始化数据包队列
    videoPacketQueue_ = std::make_shared<PacketQueue>(1000);
    audioPacketQueue_ = std::make_shared<PacketQueue>(1000);
}

RealTimeStreamRecorder::~RealTimeStreamRecorder()
{
    stopRecording();
}

bool RealTimeStreamRecorder::startRecording(const std::string &outputPath,
                                            AVFormatContext *inputFormatCtx)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (isRecording_.load()) {
        LOG_WARN("Recording is already in progress.");
        return false;
    }

    if (!inputFormatCtx) {
        LOG_ERROR("Input format context is null.");
        return false;
    }

    // 检测容器格式
    currentFormat_ = detectContainerFormat(outputPath);
    if (currentFormat_ == ContainerFormat::UNKNOWN) {
        LOG_ERROR("Unsupported container format for file: {}", outputPath);
        return false;
    }

    // 验证格式兼容性
    std::string errorMsg;
    if (!validateFormatCompatibility(currentFormat_, inputFormatCtx, errorMsg)) {
        LOG_ERROR("Format compatibility validation failed: {}", errorMsg);
        return false;
    }

    // 保存输入格式上下文引用
    inputFormatCtx_ = inputFormatCtx;

    // 初始化输出上下文
    if (!initOutputContext(outputPath, inputFormatCtx)) {
        return false;
    }

    // 创建流映射
    if (!createStreamMapping(inputFormatCtx)) {
        cleanup();
        return false;
    }

    // 启动数据包队列
    videoPacketQueue_->start();
    audioPacketQueue_->start();

    // 重置状态
    shouldStop_.store(false);
    hasKeyFrame_.store(false);

    // 启动录制线程
    isRecording_.store(true);
    recordingThread_ = std::thread(&RealTimeStreamRecorder::recordingLoop, this);

    // 发送录制开始事件
    const auto &formatInfo = getFormatInfoMap().at(currentFormat_);
    auto event = std::make_shared<RecordingEventArgs>(
        outputPath_, formatInfo.extension, "RealTimeStreamRecorder", "Recording Started");
    eventDispatcher_->triggerEvent(EventType::kRecordingStarted, event);

    LOG_INFO("Recording started: {} (Format: {})", outputPath_, formatInfo.description);
    return true;
}

bool RealTimeStreamRecorder::stopRecording()
{
    if (!isRecording_.load()) {
        return false;
    }

    // 设置停止标志
    shouldStop_.store(true);

    // 中止队列，唤醒录制线程
    videoPacketQueue_->abort();
    audioPacketQueue_->abort();

    // 通知条件变量
    cv_.notify_all();

    // 等待录制线程结束
    if (recordingThread_.joinable()) {
        recordingThread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 清理资源
    cleanup();

    isRecording_.store(false);

    // 发送录制停止事件
    auto event = std::make_shared<RecordingEventArgs>(outputPath_, "mp4", "RealTimeStreamRecorder",
                                                      "Recording Stopped");
    eventDispatcher_->triggerEvent(EventType::kRecordingStopped, event);

    LOG_INFO("Recording stopped: {}", outputPath_);
    return true;
}

bool RealTimeStreamRecorder::isRecording() const
{
    return isRecording_.load();
}

bool RealTimeStreamRecorder::writePacket(const Packet &packet, AVMediaType mediaType)
{
    if (!isRecording_.load()) {
        return false;
    }

    // 根据媒体类型选择队列
    std::shared_ptr<PacketQueue> targetQueue;
    if (mediaType == AVMEDIA_TYPE_VIDEO) {
        targetQueue = videoPacketQueue_;
    } else if (mediaType == AVMEDIA_TYPE_AUDIO) {
        targetQueue = audioPacketQueue_;
    } else {
        return false;
    }

    return targetQueue->push(packet);
}

std::string RealTimeStreamRecorder::getRecordingPath() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return outputPath_;
}

bool RealTimeStreamRecorder::initOutputContext(const std::string &outputPath,
                                               AVFormatContext *inputFormatCtx)
{
    // 获取格式信息
    const auto &formatInfo = getFormatInfoMap().at(currentFormat_);

    // 创建输出格式上下文
    int ret = avformat_alloc_output_context2(&outputFormatCtx_, nullptr,
                                             formatInfo.formatName.c_str(), outputPath.c_str());
    if (ret < 0 || !outputFormatCtx_) {
        LOG_ERROR("Failed to allocate output format context for {}: {}", formatInfo.description,
                  utils::avErr2Str(ret));
        return false;
    }

    // 复制流信息
    for (unsigned int i = 0; i < inputFormatCtx->nb_streams; i++) {
        AVStream *inStream = inputFormatCtx->streams[i];

        // 只处理视频和音频流
        if (inStream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            inStream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }

        // 检查编解码器兼容性
        const char *codecName = avcodec_get_name(inStream->codecpar->codec_id);
        bool codecSupported = false;

        if (inStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            codecSupported = std::find(formatInfo.supportedVideoCodecs.begin(),
                                       formatInfo.supportedVideoCodecs.end(),
                                       codecName) != formatInfo.supportedVideoCodecs.end();
        } else if (inStream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            codecSupported = std::find(formatInfo.supportedAudioCodecs.begin(),
                                       formatInfo.supportedAudioCodecs.end(),
                                       codecName) != formatInfo.supportedAudioCodecs.end();
        }

        if (!codecSupported) {
            LOG_WARN("Codec {} may not be fully supported by {} format", codecName,
                     formatInfo.description);
        }

        // 创建输出流
        AVStream *outStream = avformat_new_stream(outputFormatCtx_, nullptr);
        if (!outStream) {
            LOG_ERROR("Failed to allocate output stream");
            return false;
        }

        // 复制编解码器参数
        ret = avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
        if (ret < 0) {
            LOG_ERROR("Failed to copy codec parameters: {}", utils::avErr2Str(ret));
            return false;
        }

        outStream->codecpar->codec_tag = 0;
    }

    // 打开输出文件
    if (!(outputFormatCtx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&outputFormatCtx_->pb, utils::convertPathForFFmpeg(outputPath).c_str(),
                        AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOG_ERROR("Failed to open output file {}: {}", outputPath, utils::avErr2Str(ret));
            return false;
        }
    }

    // 写入文件头
    ret = avformat_write_header(outputFormatCtx_, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to write header: {}", utils::avErr2Str(ret));
        return false;
    }

    outputPath_ = outputPath;
    return true;
}

bool RealTimeStreamRecorder::createStreamMapping(AVFormatContext *inputFormatCtx)
{
    streamCount_ = inputFormatCtx->nb_streams;
    streamMapping_ = static_cast<int *>(av_malloc_array(streamCount_, sizeof(int)));
    if (!streamMapping_) {
        LOG_ERROR("Failed to allocate stream mapping array");
        return false;
    }

    int outputStreamIndex = 0;
    for (int i = 0; i < streamCount_; i++) {
        AVStream *inStream = inputFormatCtx->streams[i];
        if (inStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
            inStream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            streamMapping_[i] = outputStreamIndex++;
        } else {
            streamMapping_[i] = -1;
        }
    }

    return true;
}

void RealTimeStreamRecorder::recordingLoop()
{
    AVPacket *tempPacket = av_packet_alloc();
    if (!tempPacket) {
        LOG_ERROR("Failed to allocate temporary packet");
        return;
    }

    while (!shouldStop_.load()) {
        bool hasData = false;

        // 处理视频数据包
        Packet videoPacket;
        if (videoPacketQueue_->pop(videoPacket, 1)) {
            if (processPacket(videoPacket, AVMEDIA_TYPE_VIDEO)) {
                hasData = true;
            }
        }

        // 处理音频数据包（只有在有关键帧后才处理）
        if (hasKeyFrame_.load()) {
            Packet audioPacket;
            if (audioPacketQueue_->pop(audioPacket, 1)) {
                if (processPacket(audioPacket, AVMEDIA_TYPE_AUDIO)) {
                    hasData = true;
                }
            }
        }

        // 如果没有数据，短暂休眠
        if (!hasData) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    av_packet_free(&tempPacket);
}

bool RealTimeStreamRecorder::processPacket(const Packet &packet, AVMediaType mediaType)
{
    if (!outputFormatCtx_ || !inputFormatCtx_ || !streamMapping_) {
        return false;
    }

    AVPacket *pkt = packet.get();
    if (!pkt || pkt->stream_index >= streamCount_) {
        return false;
    }

    // 检查视频关键帧
    if (mediaType == AVMEDIA_TYPE_VIDEO) {
        if (!hasKeyFrame_.load() && (pkt->flags & AV_PKT_FLAG_KEY) == 0) {
            return false; // 等待关键帧
        }
        if (!hasKeyFrame_.load()) {
            hasKeyFrame_.store(true);
        }
    }

    // 获取输出流索引
    int outStreamIndex = streamMapping_[pkt->stream_index];
    if (outStreamIndex < 0) {
        return false;
    }

    // 获取输入和输出流
    AVStream *inStream = inputFormatCtx_->streams[pkt->stream_index];
    AVStream *outStream = outputFormatCtx_->streams[outStreamIndex];

    if (!inStream || !outStream) {
        LOG_ERROR("Invalid input or output stream");
        return false;
    }

    // 查看当前是否创建了这个类型的初始时间戳，如果没保存，则先保存
    if (firstPtsMap_.find(mediaType) == firstPtsMap_.end()) {
        // 保存初始PTS和DTS（输入流时间基）
        int64_t firstPts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : 0;
        int64_t firstDts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : firstPts;

        firstPtsMap_.insert({mediaType, firstPts});
        firstDtsMap_.insert({mediaType, firstDts});

        LOG_DEBUG("Saved first timestamps for media type {}: PTS={}, DTS={}",
                  static_cast<int>(mediaType), firstPts, firstDts);
    }

    // 创建临时数据包
    AVPacket *tempPacket = av_packet_alloc();
    if (!tempPacket) {
        return false;
    }

    // 复制数据包
    int ret = av_packet_ref(tempPacket, pkt);
    if (ret < 0) {
        av_packet_free(&tempPacket);
        return false;
    }

    // 调整流索引
    tempPacket->stream_index = outStreamIndex;

    // 时间戳转换：从输入流时间基转换到输出流时间基
    int64_t firstPts = firstPtsMap_[mediaType];
    int64_t firstDts = firstDtsMap_[mediaType];

    // 1. 先转换为相对时间戳（相对于第一帧）
    if (tempPacket->pts != AV_NOPTS_VALUE) {
        tempPacket->pts -= firstPts;
        // 确保PTS不为负数
        if (tempPacket->pts < 0) {
            tempPacket->pts = 0;
        }
    }
    if (tempPacket->dts != AV_NOPTS_VALUE) {
        tempPacket->dts -= firstDts;
        // 确保DTS不为负数
        if (tempPacket->dts < 0) {
            tempPacket->dts = 0;
        }
    }

    // 2. 从输入流时间基转换到输出流时间基
    if (tempPacket->pts != AV_NOPTS_VALUE) {
        tempPacket->pts = av_rescale_q(tempPacket->pts, inStream->time_base, outStream->time_base);
    }
    if (tempPacket->dts != AV_NOPTS_VALUE) {
        tempPacket->dts = av_rescale_q(tempPacket->dts, inStream->time_base, outStream->time_base);
    }
    if (tempPacket->duration > 0) {
        tempPacket->duration =
            av_rescale_q(tempPacket->duration, inStream->time_base, outStream->time_base);
    }

    // 确保DTS <= PTS
    if (tempPacket->dts != AV_NOPTS_VALUE && tempPacket->pts != AV_NOPTS_VALUE) {
        if (tempPacket->dts > tempPacket->pts) {
            LOG_WARN("DTS ({}) > PTS ({}), adjusting DTS to PTS", tempPacket->dts, tempPacket->pts);
            tempPacket->dts = tempPacket->pts;
        }
    }

    // // 确保时间戳单调递增（针对每种媒体类型）
    // static std::unordered_map<AVMediaType, int64_t> lastPtsMap;
    // static std::unordered_map<AVMediaType, int64_t> lastDtsMap;

    // if (tempPacket->pts != AV_NOPTS_VALUE) {
    //     auto it = lastPtsMap.find(mediaType);
    //     if (it != lastPtsMap.end() && tempPacket->pts <= it->second) {
    //         tempPacket->pts = it->second + 1;
    //         LOG_DEBUG("Adjusted PTS for monotonicity: {}", tempPacket->pts);
    //     }
    //     lastPtsMap[mediaType] = tempPacket->pts;
    // }

    // if (tempPacket->dts != AV_NOPTS_VALUE) {
    //     auto it = lastDtsMap.find(mediaType);
    //     if (it != lastDtsMap.end() && tempPacket->dts <= it->second) {
    //         tempPacket->dts = it->second + 1;
    //         LOG_DEBUG("Adjusted DTS for monotonicity: {}", tempPacket->dts);
    //     }
    //     lastDtsMap[mediaType] = tempPacket->dts;

    //     // 再次确保DTS <= PTS
    //     if (tempPacket->pts != AV_NOPTS_VALUE && tempPacket->dts > tempPacket->pts) {
    //         tempPacket->dts = tempPacket->pts;
    //     }
    // }

    // 写入数据包
    ret = av_interleaved_write_frame(outputFormatCtx_, tempPacket);
    if (ret < 0) {
        const auto errStr = utils::avErr2Str(ret);
        LOG_ERROR("Failed to write frame: PTS={}, DTS={}, error: {}", tempPacket->pts,
                  tempPacket->dts, errStr);
        av_packet_unref(tempPacket);
        av_packet_free(&tempPacket);

        const auto event = std::make_shared<RecordingEventArgs>(
            outputPath_, "mp4", "RealTimeStreamRecorder", "Recording Error", ret, errStr);
        eventDispatcher_->triggerEvent(EventType::kRecordingError, event);

        return false;
    }

    av_packet_unref(tempPacket);
    av_packet_free(&tempPacket);
    return true;
}

void RealTimeStreamRecorder::cleanup()
{
    // 写入文件尾
    if (outputFormatCtx_) {
        av_write_trailer(outputFormatCtx_);

        // 关闭输出文件
        if (!(outputFormatCtx_->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&outputFormatCtx_->pb);
        }

        // 释放输出格式上下文
        avformat_free_context(outputFormatCtx_);
        outputFormatCtx_ = nullptr;
    }

    // 清空输入格式上下文引用
    inputFormatCtx_ = nullptr;

    // 释放流映射表
    if (streamMapping_) {
        av_free(streamMapping_);
        streamMapping_ = nullptr;
    }

    // 清空队列
    if (videoPacketQueue_) {
        videoPacketQueue_->flush();
    }
    if (audioPacketQueue_) {
        audioPacketQueue_->flush();
    }

    // 清空首帧PTS和DTS映射
    firstPtsMap_.clear();
    firstDtsMap_.clear();

    outputPath_.clear();
    streamCount_ = 0;
    currentFormat_ = ContainerFormat::UNKNOWN;
}

std::vector<ContainerFormatInfo> RealTimeStreamRecorder::getSupportedFormats()
{
    std::vector<ContainerFormatInfo> formats;
    const auto &formatMap = getFormatInfoMap();

    for (const auto &pair : formatMap) {
        if (pair.first != ContainerFormat::UNKNOWN) {
            formats.push_back(pair.second);
        }
    }

    return formats;
}

ContainerFormat RealTimeStreamRecorder::detectContainerFormat(const std::string &filePath)
{
    // 获取文件扩展名
    std::filesystem::path path(filePath);
    std::string extension = path.extension().string();

    // 转换为小写
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    // 移除点号
    if (!extension.empty() && extension[0] == '.') {
        extension = extension.substr(1);
    }

    // 匹配扩展名
    const auto &formatMap = getFormatInfoMap();
    for (const auto &pair : formatMap) {
        if (pair.second.extension == extension) {
            return pair.first;
        }
    }

    return ContainerFormat::UNKNOWN;
}

bool RealTimeStreamRecorder::validateFormatCompatibility(ContainerFormat format,
                                                         AVFormatContext *inputFormatCtx,
                                                         std::string &errorMsg)
{
    if (format == ContainerFormat::UNKNOWN) {
        errorMsg = "Unknown container format";
        return false;
    }

    if (!inputFormatCtx) {
        errorMsg = "Input format context is null";
        return false;
    }

    const auto &formatInfo = getFormatInfoMap().at(format);

    bool hasVideo = false;
    bool hasAudio = false;
    std::vector<std::string> unsupportedCodecs;

    // 检查每个流
    for (unsigned int i = 0; i < inputFormatCtx->nb_streams; i++) {
        AVStream *stream = inputFormatCtx->streams[i];
        const char *codecName = avcodec_get_name(stream->codecpar->codec_id);

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            hasVideo = true;
            if (std::find(formatInfo.supportedVideoCodecs.begin(),
                          formatInfo.supportedVideoCodecs.end(),
                          codecName) == formatInfo.supportedVideoCodecs.end()) {
                unsupportedCodecs.push_back(std::string("Video: ") + codecName);
            }
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            hasAudio = true;
            if (std::find(formatInfo.supportedAudioCodecs.begin(),
                          formatInfo.supportedAudioCodecs.end(),
                          codecName) == formatInfo.supportedAudioCodecs.end()) {
                unsupportedCodecs.push_back(std::string("Audio: ") + codecName);
            }
        }
    }

    // 检查格式是否支持视频/音频
    if (hasVideo && !formatInfo.supportVideo) {
        errorMsg = formatInfo.description + " format does not support video streams";
        return false;
    }

    if (hasAudio && !formatInfo.supportAudio) {
        errorMsg = formatInfo.description + " format does not support audio streams";
        return false;
    }

    // 检查不支持的编解码器
    if (!unsupportedCodecs.empty()) {
        errorMsg = "Unsupported codecs for " + formatInfo.description + " format: ";
        for (size_t i = 0; i < unsupportedCodecs.size(); ++i) {
            if (i > 0)
                errorMsg += ", ";
            errorMsg += unsupportedCodecs[i];
        }
        return false;
    }

    return true;
}

const std::unordered_map<ContainerFormat, ContainerFormatInfo> &
RealTimeStreamRecorder::getFormatInfoMap()
{
    static const std::unordered_map<ContainerFormat, ContainerFormatInfo> formatMap = {
        {ContainerFormat::MP4,
         {ContainerFormat::MP4,
          "mp4",
          "mp4",
          "MPEG-4 Part 14",
          true,
          true,
          {"h264", "h265", "hevc", "mpeg4", "av1"},
          {"aac", "mp3", "ac3", "eac3", "opus"}}},
        {ContainerFormat::AVI,
         {ContainerFormat::AVI,
          "avi",
          "avi",
          "Audio Video Interleave",
          true,
          true,
          {"h264", "mpeg4", "mjpeg", "rawvideo"},
          {"mp3", "ac3", "pcm_s16le", "pcm_s24le"}}},
        {ContainerFormat::MKV,
         {ContainerFormat::MKV,
          "matroska",
          "mkv",
          "Matroska",
          true,
          true,
          {"h264", "h265", "hevc", "vp8", "vp9", "av1", "mpeg4"},
          {"aac", "mp3", "ac3", "eac3", "opus", "vorbis", "flac", "pcm_s16le"}}},
        {ContainerFormat::MOV,
         {ContainerFormat::MOV,
          "mov",
          "mov",
          "QuickTime",
          true,
          true,
          {"h264", "h265", "hevc", "mpeg4", "prores"},
          {"aac", "mp3", "ac3", "pcm_s16le", "pcm_s24le"}}},
        {ContainerFormat::FLV,
         {ContainerFormat::FLV,
          "flv",
          "flv",
          "Flash Video",
          true,
          true,
          {"h264", "flv1"},
          {"aac", "mp3", "pcm_s16le"}}},
        {ContainerFormat::TS,
         {ContainerFormat::TS,
          "mpegts",
          "ts",
          "MPEG Transport Stream",
          true,
          true,
          {"h264", "h265", "hevc", "mpeg2video"},
          {"aac", "mp3", "ac3", "eac3"}}},
        {ContainerFormat::WEBM,
         {ContainerFormat::WEBM,
          "webm",
          "webm",
          "WebM",
          true,
          true,
          {"vp8", "vp9", "av1"},
          {"vorbis", "opus"}}},
        {ContainerFormat::OGV,
         {ContainerFormat::OGV,
          "ogg",
          "ogv",
          "Ogg Video",
          true,
          true,
          {"theora", "vp8"},
          {"vorbis", "opus", "flac"}}},
        {ContainerFormat::UNKNOWN,
         {ContainerFormat::UNKNOWN, "", "", "Unknown Format", false, false, {}, {}}}};

    return formatMap;
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END