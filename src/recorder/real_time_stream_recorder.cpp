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

namespace {
constexpr char kModuleName[] = "Real Time Stream Recorder";
}

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
                                            AVFormatContext *inputFormatCtx,
                                            MediaTypes recordMediaTypes)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (isRecording_.load()) {
        LOG_WARN("Recording is already in progress.");
        return false;
    }

    if (!inputFormatCtx) {
        static char errStr[] = "Input format context is null.";
        LOG_ERROR(errStr);
        triggerErrorEvent(outputPath, ContainerFormat::UNKNOWN, -1, errStr);
        return false;
    }

    // 检测容器格式
    currentFormat_ = detectContainerFormat(outputPath);
    if (currentFormat_ == ContainerFormat::UNKNOWN) {
        const std::string errStr = "Unsupported container format for file: " + outputPath;
        LOG_ERROR(errStr.c_str());
        triggerErrorEvent(outputPath, currentFormat_, -2, errStr);
        return false;
    }

    // 验证格式兼容性
    std::string errorMsg;
    if (!validateFormatCompatibility(currentFormat_, inputFormatCtx, recordMediaTypes, errorMsg)) {
        const std::string errStr = "Format compatibility validation failed: " + errorMsg;
        LOG_ERROR(errStr.c_str());
        triggerErrorEvent(outputPath, currentFormat_, -3, errStr);
        return false;
    }

    // 保存输入格式上下文引用
    inputFormatCtx_ = inputFormatCtx;

    // 初始化输出上下文
    if (int ret = initOutputContext(outputPath, inputFormatCtx, recordMediaTypes); ret != 0) {
        const std::string errStr = "Init output context failed: " + utils::avErr2Str(ret);
        LOG_ERROR(errStr.c_str());
        triggerErrorEvent(outputPath, currentFormat_, ret, errStr);
        return false;
    }

    // 创建流映射
    if (!createStreamMapping(inputFormatCtx, recordMediaTypes)) {
        cleanup();
        const std::string errStr =
            "Init output context failed: " + utils::avErr2Str(AVERROR(ENOMEM));
        LOG_ERROR(errStr.c_str());
        triggerErrorEvent(outputPath, currentFormat_, AVERROR(ENOMEM), errStr);
        return false;
    }

    // 刷新预缓冲数据到录制文件（报警前画面）
    // 注意：刷新后不清空预缓冲区也不禁用预缓冲，预缓冲继续运行，
    // 这样下一次 startRecording() 被调用时（如连续报警），仍能获取到最近 N 秒的数据。
    flushPrebufferToRecording();

    // 启动数据包队列
    videoPacketQueue_->start();
    audioPacketQueue_->start();

    // 重置状态
    shouldStop_.store(false);

    // 启动录制线程
    isRecording_.store(true);
    recordingThread_ = std::thread(&RealTimeStreamRecorder::recordingLoop, this);

    // 发送录制开始事件
    const auto &formatInfo = getFormatInfoMap().at(currentFormat_);
    auto event =
        std::make_shared<RecordingEventArgs>(outputPath_, formatInfo.extension, kModuleName,
                                             utils::eventType2Desc(EventType::kRecordingStarted));
    eventDispatcher_->triggerEvent(EventType::kRecordingStarted, event);

    LOG_INFO("Recording started: {} (Format: {})", outputPath_, formatInfo.description);
    return true;
}

bool RealTimeStreamRecorder::stopRecording()
{
    std::lock_guard<std::mutex> lock(mutex_);

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

    // 记录当前的format info和输出路径
    const auto &formatInfo = getFormatInfoMap().at(currentFormat_);
    const std::string outputPath = outputPath_;

    // 刷新音频转码器，写入剩余数据
    flushAudioTranscoder();

    // 清理资源
    cleanup();

    isRecording_.store(false);

    // 发送录制停止事件
    auto event =
        std::make_shared<RecordingEventArgs>(outputPath, formatInfo.extension, kModuleName,
                                             utils::eventType2Desc(EventType::kRecordingStopped));
    eventDispatcher_->triggerEvent(EventType::kRecordingStopped, event);
    LOG_INFO("Recording stopped: {}", outputPath);
    return true;
}

bool RealTimeStreamRecorder::isRecording() const
{
    return isRecording_.load();
}

bool RealTimeStreamRecorder::writePacket(const Packet &packet, AVMediaType mediaType)
{
    // 预缓冲模式：无论是否处于录制状态，都缓存数据包
    if (prebufferDurationMs_ > 0) {
        std::lock_guard<std::mutex> lock(prebufferMutex_);
        prebuffer_.push_back({packet, mediaType});

        // 累加时长（微秒），用于淘汰旧包
        if (packet.duration() > 0) {
            prebufferTotalDurationUs_ += packet.duration();
        }

        // 淘汰超出预缓冲时长的旧包
        const int64_t maxDurationUs = prebufferDurationMs_ * 1000;
        while (!prebuffer_.empty() && prebufferTotalDurationUs_ > maxDurationUs) {
            const auto &front = prebuffer_.front();
            if (front.packet.duration() > 0) {
                prebufferTotalDurationUs_ -= front.packet.duration();
            }
            prebuffer_.pop_front();
        }
    }

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

void RealTimeStreamRecorder::setPrebufferDuration(int64_t durationMs)
{
    std::lock_guard<std::mutex> lock(prebufferMutex_);
    prebufferDurationMs_ = durationMs;

    // 如果新时长为 0，清空缓冲区
    if (durationMs <= 0) {
        prebuffer_.clear();
        prebufferTotalDurationUs_ = 0;
        return;
    }

    // 如果新时长缩小，立即淘汰多余数据
    const int64_t maxDurationUs = durationMs * 1000;
    while (!prebuffer_.empty() && prebufferTotalDurationUs_ > maxDurationUs) {
        const auto &front = prebuffer_.front();
        if (front.packet.duration() > 0) {
            prebufferTotalDurationUs_ -= front.packet.duration();
        }
        prebuffer_.pop_front();
    }
}

void RealTimeStreamRecorder::clearPrebuffer()
{
    std::lock_guard<std::mutex> lock(prebufferMutex_);
    prebuffer_.clear();
    prebufferTotalDurationUs_ = 0;
}

void RealTimeStreamRecorder::flushPrebufferToRecording()
{
    // 拷贝预缓冲数据而非 swap：预缓冲是持续维护的环形队列，必须保持不变，
    // 这样下一次报警触发时，缓冲区里仍然有最近 N 秒的完整数据。
    std::deque<TimestampedPacket> buffered;
    {
        std::lock_guard<std::mutex> lock(prebufferMutex_);
        buffered = prebuffer_;  // 拷贝，不清空预缓冲
    }

    if (buffered.empty()) {
        return;
    }

    LOG_INFO("Flushing {} prebuffer packets to recording file.", buffered.size());

    bool hasKeyFrame = false;
    for (const auto &tp : buffered) {
        if (!tp.packet.isValid()) {
            continue;
        }
        processPacket(tp.packet, tp.mediaType, hasKeyFrame);
    }
}

std::string RealTimeStreamRecorder::getRecordingPath() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return outputPath_;
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
                                                         MediaTypes recordMediaTypes,
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

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            recordMediaTypes.has(MediaType::kVideo)) {
            hasVideo = true;
            if (std::find(formatInfo.supportedVideoCodecs.begin(),
                          formatInfo.supportedVideoCodecs.end(),
                          codecName) == formatInfo.supportedVideoCodecs.end()) {
                unsupportedCodecs.push_back(std::string("Video: ") + codecName);
            }
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                   recordMediaTypes.has(MediaType::kAudio)) {
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
        // 检查是否可以通过转码解决（目前仅支持音频转码）
        bool allTranscodable = true;
        for (const auto &codec : unsupportedCodecs) {
            if (codec.find("Audio: ") != 0) {
                allTranscodable = false;
                break;
            }
        }

        if (allTranscodable) {
            // 音频编解码器不被支持，但可以转码为 AAC
            LOG_INFO("Audio codec(s) not natively supported by {}, will transcode to AAC",
                     formatInfo.description);
            return true;
        }

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

void RealTimeStreamRecorder::recordingLoop()
{
    bool hasKeyFrame = false;
    while (!shouldStop_.load()) {
        bool hasData = false;

        // 处理视频数据包
        Packet videoPacket;
        if (videoPacketQueue_->pop(videoPacket, 1)) {
            if (processPacket(videoPacket, AVMEDIA_TYPE_VIDEO, hasKeyFrame)) {
                hasData = true;
            }
        }

        // 处理音频数据包（只有在有关键帧后才处理）
        if (hasKeyFrame) {
            Packet audioPacket;
            if (audioPacketQueue_->pop(audioPacket, 1)) {
                if (processPacket(audioPacket, AVMEDIA_TYPE_AUDIO, hasKeyFrame)) {
                    hasData = true;
                }
            }
        }

        // 如果没有数据，短暂休眠
        if (!hasData) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

int RealTimeStreamRecorder::initOutputContext(const std::string &outputPath,
                                              AVFormatContext *inputFormatCtx,
                                              MediaTypes recordMediaTypes)
{
    // 获取格式信息
    const auto &formatInfo = getFormatInfoMap().at(currentFormat_);

    // 创建输出格式上下文
    int ret = avformat_alloc_output_context2(&outputFormatCtx_, nullptr,
                                             formatInfo.formatName.c_str(), outputPath.c_str());
    if (ret < 0 || !outputFormatCtx_) {
        LOG_ERROR("Failed to allocate output format context for {}: {}", formatInfo.description,
                  utils::avErr2Str(ret));
        return ret;
    }

    // 复制流信息
    for (unsigned int i = 0; i < inputFormatCtx->nb_streams; i++) {
        AVStream *inStream = inputFormatCtx->streams[i];

        // 是否应该被处理
        bool shouldProcess = false;
        if (inStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            recordMediaTypes.has(MediaType::kVideo)) {
            shouldProcess = true;
        } else if (inStream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                   recordMediaTypes.has(MediaType::kAudio)) {
            shouldProcess = true;
        }

        // 只处理应该被处理的流
        if (!shouldProcess) {
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
            return AVERROR(ENOMEM);
        }

        // 复制编解码器参数
        ret = avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
        if (ret < 0) {
            LOG_ERROR("Failed to copy codec parameters: {}", utils::avErr2Str(ret));
            return ret;
        }

        outStream->codecpar->codec_tag = 0;

        // 复制时间基
        outStream->time_base = inStream->time_base;

        // 复制帧率信息
        outStream->r_frame_rate = inStream->r_frame_rate;
        outStream->avg_frame_rate = inStream->avg_frame_rate;

        // 如果音频编解码器不被支持，初始化解码器进行转码
        if (!codecSupported && inStream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (!initAudioTranscoder(inStream, outStream)) {
                LOG_ERROR("Failed to init audio transcoder for codec: {}", codecName);
                return AVERROR(ENOMEM);
            }

            // 更新输出流的编解码器参数为 AAC（转码目标）
            outStream->codecpar->codec_id = AV_CODEC_ID_AAC;
            outStream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            outStream->codecpar->sample_rate = audioTranscoder_->encoderCtx->sample_rate;
#if LIBAVCODEC_VERSION_MAJOR >= 60
            av_channel_layout_copy(&outStream->codecpar->ch_layout,
                                   &audioTranscoder_->encoderCtx->ch_layout);
#else
            outStream->codecpar->channels = audioTranscoder_->encoderCtx->channels;
            outStream->codecpar->channel_layout = audioTranscoder_->encoderCtx->channel_layout;
#endif
            outStream->codecpar->bit_rate = audioTranscoder_->encoderCtx->bit_rate;
            outStream->codecpar->format = audioTranscoder_->encoderCtx->sample_fmt;
            outStream->codecpar->frame_size = audioTranscoder_->encoderCtx->frame_size;
            outStream->codecpar->codec_tag = 0;
        }
    }

    // 打开输出文件
    if (!(outputFormatCtx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&outputFormatCtx_->pb, utils::convertPathForFFmpeg(outputPath).c_str(),
                        AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOG_ERROR("Failed to open output file {}: {}", outputPath, utils::avErr2Str(ret));
            return ret;
        }
    }

    // 写入文件头
    ret = avformat_write_header(outputFormatCtx_, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to write header: {}", utils::avErr2Str(ret));
        return ret;
    }

    outputPath_ = outputPath;
    return 0;
}

void RealTimeStreamRecorder::cleanup()
{
    // 写入文件尾
    if (outputFormatCtx_) {
        int ret = av_write_trailer(outputFormatCtx_);
        if (ret < 0) {
            LOG_ERROR("Failed to write trailer: {}", utils::avErr2Str(ret));
        }

        // 关闭输出文件
        if (!(outputFormatCtx_->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_closep(&outputFormatCtx_->pb);
            if (ret < 0) {
                LOG_ERROR("Failed to close output file: {}", utils::avErr2Str(ret));
            }
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

    // 释放音频转码资源
    cleanupAudioTranscoder();

    outputPath_.clear();
    streamCount_ = 0;
    currentFormat_ = ContainerFormat::UNKNOWN;
}

bool RealTimeStreamRecorder::createStreamMapping(AVFormatContext *inputFormatCtx,
                                                 MediaTypes recordMediaTypes)
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
        if (inStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            recordMediaTypes.has(MediaType::kVideo)) {
            streamMapping_[i] = outputStreamIndex++;
        } else if (inStream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                   recordMediaTypes.has(MediaType::kAudio)) {
            streamMapping_[i] = outputStreamIndex++;
        } else {
            streamMapping_[i] = -1;
        }
    }

    return true;
}

bool RealTimeStreamRecorder::processPacket(const Packet &packet, AVMediaType mediaType,
                                           bool &hasKeyFrame)
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
        if (!hasKeyFrame && (pkt->flags & AV_PKT_FLAG_KEY) == 0) {
            return false; // 等待关键帧
        }
        if (!hasKeyFrame) {
            hasKeyFrame = true;
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

    // 如果需要音频转码，使用转码路径
    if (mediaType == AVMEDIA_TYPE_AUDIO && audioTranscoder_ && audioTranscoder_->initialized) {
        return transcodeAndWriteAudioPacket(pkt, outStream);
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
        triggerErrorEvent(outputPath_, currentFormat_, AVERROR(ENOMEM),
                          utils::avErr2Str(AVERROR(ENOMEM)));
        return false;
    }

    // 复制数据包
    int ret = av_packet_ref(tempPacket, pkt);
    if (ret < 0) {
        av_packet_free(&tempPacket);
        triggerErrorEvent(outputPath_, currentFormat_, ret, utils::avErr2Str(ret));
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

        triggerErrorEvent(outputPath_, currentFormat_, ret, errStr);
        return false;
    }

    av_packet_unref(tempPacket);
    av_packet_free(&tempPacket);
    return true;
}

void RealTimeStreamRecorder::triggerErrorEvent(const std::string &outputPath,
                                               ContainerFormat format, int ret,
                                               const std::string &errStr)
{
    const auto &formatInfo = getFormatInfoMap().at(format);
    const auto event = std::make_shared<RecordingEventArgs>(
        outputPath, formatInfo.extension, kModuleName,
        utils::eventType2Desc(EventType::kRecordingError), ret, errStr);
    eventDispatcher_->triggerEvent(EventType::kRecordingError, event);
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

bool RealTimeStreamRecorder::initAudioTranscoder(AVStream *inStream, AVStream *outStream)
{
    if (!inStream || !outStream) {
        return false;
    }

    // 创建转码器状态
    audioTranscoder_ = std::make_unique<AudioTranscoderState>();

    // 查找输入音频解码器
    const AVCodec *decoderCodec = avcodec_find_decoder(inStream->codecpar->codec_id);
    if (!decoderCodec) {
        LOG_ERROR("Failed to find decoder for codec: {}",
                  avcodec_get_name(inStream->codecpar->codec_id));
        return false;
    }

    // 查找 AAC 编码器
    const AVCodec *encoderCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!encoderCodec) {
        LOG_ERROR("Failed to find AAC encoder");
        return false;
    }

    // 创建解码器上下文
    audioTranscoder_->decoderCtx = avcodec_alloc_context3(decoderCodec);
    if (!audioTranscoder_->decoderCtx) {
        LOG_ERROR("Failed to allocate decoder context");
        return false;
    }

    // 复制输入编解码器参数到解码器上下文
    int ret = avcodec_parameters_to_context(audioTranscoder_->decoderCtx, inStream->codecpar);
    if (ret < 0) {
        LOG_ERROR("Failed to copy codec parameters to decoder: {}", utils::avErr2Str(ret));
        return false;
    }

    ret = avcodec_open2(audioTranscoder_->decoderCtx, decoderCodec, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to open decoder: {}", utils::avErr2Str(ret));
        return false;
    }

    // 创建编码器上下文
    audioTranscoder_->encoderCtx = avcodec_alloc_context3(encoderCodec);
    if (!audioTranscoder_->encoderCtx) {
        LOG_ERROR("Failed to allocate encoder context");
        return false;
    }

    // 配置编码器参数
    audioTranscoder_->encoderCtx->sample_rate = audioTranscoder_->decoderCtx->sample_rate;
#if LIBAVCODEC_VERSION_MAJOR >= 60
    av_channel_layout_copy(&audioTranscoder_->encoderCtx->ch_layout,
                           &audioTranscoder_->decoderCtx->ch_layout);
#else
    audioTranscoder_->encoderCtx->channels = audioTranscoder_->decoderCtx->channels;
    audioTranscoder_->encoderCtx->channel_layout = audioTranscoder_->decoderCtx->channel_layout;
#endif
    audioTranscoder_->encoderCtx->sample_fmt = encoderCodec->sample_fmts[0];
    audioTranscoder_->encoderCtx->bit_rate = 128000;  // 128kbps
    audioTranscoder_->encoderCtx->time_base = {1, audioTranscoder_->encoderCtx->sample_rate};

    ret = avcodec_open2(audioTranscoder_->encoderCtx, encoderCodec, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to open encoder: {}", utils::avErr2Str(ret));
        return false;
    }

    // 创建重采样上下文
    ret = swr_alloc_set_opts2(&audioTranscoder_->swrCtx,
#if LIBAVCODEC_VERSION_MAJOR >= 60
                              &audioTranscoder_->encoderCtx->ch_layout,
#else
                              &audioTranscoder_->encoderCtx->channel_layout,
#endif
                              audioTranscoder_->encoderCtx->sample_fmt,
                              audioTranscoder_->encoderCtx->sample_rate,
#if LIBAVCODEC_VERSION_MAJOR >= 60
                              &audioTranscoder_->decoderCtx->ch_layout,
#else
                              &audioTranscoder_->decoderCtx->channel_layout,
#endif
                              audioTranscoder_->decoderCtx->sample_fmt,
                              audioTranscoder_->decoderCtx->sample_rate, 0, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to allocate resampler: {}", utils::avErr2Str(ret));
        return false;
    }

    ret = swr_init(audioTranscoder_->swrCtx);
    if (ret < 0) {
        LOG_ERROR("Failed to init resampler: {}", utils::avErr2Str(ret));
        return false;
    }

    // 创建 FIFO 缓冲
    audioTranscoder_->audioFifo = av_audio_fifo_alloc(audioTranscoder_->encoderCtx->sample_fmt,
#if LIBAVCODEC_VERSION_MAJOR >= 60
                                                       audioTranscoder_->encoderCtx->ch_layout.nb_channels,
#else
                                                       audioTranscoder_->encoderCtx->channels,
#endif
                                                       1);
    if (!audioTranscoder_->audioFifo) {
        LOG_ERROR("Failed to allocate audio FIFO");
        return false;
    }

    // 分配重采样输出帧和 FIFO 读出帧
    audioTranscoder_->resampleFrame = av_frame_alloc();
    audioTranscoder_->fifoFrame = av_frame_alloc();
    if (!audioTranscoder_->resampleFrame || !audioTranscoder_->fifoFrame) {
        LOG_ERROR("Failed to allocate frames");
        return false;
    }

    // 配置 FIFO 读出帧
    audioTranscoder_->fifoFrame->nb_samples = audioTranscoder_->encoderCtx->frame_size;
    audioTranscoder_->fifoFrame->format = audioTranscoder_->encoderCtx->sample_fmt;
#if LIBAVCODEC_VERSION_MAJOR >= 60
    av_channel_layout_copy(&audioTranscoder_->fifoFrame->ch_layout,
                           &audioTranscoder_->encoderCtx->ch_layout);
#else
    audioTranscoder_->fifoFrame->channel_layout = audioTranscoder_->encoderCtx->channel_layout;
    audioTranscoder_->fifoFrame->channels = audioTranscoder_->encoderCtx->channels;
#endif
    audioTranscoder_->fifoFrame->sample_rate = audioTranscoder_->encoderCtx->sample_rate;

    ret = av_frame_get_buffer(audioTranscoder_->fifoFrame, 0);
    if (ret < 0) {
        LOG_ERROR("Failed to allocate FIFO frame buffer: {}", utils::avErr2Str(ret));
        return false;
    }

    audioTranscoder_->initialized = true;
    audioTranscoder_->pts = 0;

    LOG_INFO("Audio transcoder initialized: {} -> aac",
             avcodec_get_name(inStream->codecpar->codec_id));
    return true;
}

bool RealTimeStreamRecorder::transcodeAndWriteAudioPacket(AVPacket *packet, AVStream *outStream)
{
    if (!audioTranscoder_ || !audioTranscoder_->initialized) {
        return false;
    }

    // 发送数据包到解码器
    int ret = avcodec_send_packet(audioTranscoder_->decoderCtx, packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        LOG_ERROR("Failed to send packet to decoder: {}", utils::avErr2Str(ret));
        return false;
    }

    // 接收解码后的帧
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        return false;
    }

    ret = avcodec_receive_frame(audioTranscoder_->decoderCtx, frame);
    if (ret < 0) {
        av_frame_free(&frame);
        if (ret == AVERROR(EAGAIN)) {
            return true;  // 需要更多数据，不是错误
        }
        LOG_ERROR("Failed to receive frame from decoder: {}", utils::avErr2Str(ret));
        return false;
    }

    // 重采样
    int64_t delay = swr_get_delay(audioTranscoder_->swrCtx, frame->sample_rate);
    int64_t outSamples = av_rescale_rnd(delay + frame->nb_samples,
                                        audioTranscoder_->encoderCtx->sample_rate,
                                        frame->sample_rate, AV_ROUND_UP);

    av_frame_unref(audioTranscoder_->resampleFrame);
    audioTranscoder_->resampleFrame->format = audioTranscoder_->encoderCtx->sample_fmt;
#if LIBAVCODEC_VERSION_MAJOR >= 60
    av_channel_layout_copy(&audioTranscoder_->resampleFrame->ch_layout,
                           &audioTranscoder_->encoderCtx->ch_layout);
#else
    audioTranscoder_->resampleFrame->channel_layout = audioTranscoder_->encoderCtx->channel_layout;
    audioTranscoder_->resampleFrame->channels = audioTranscoder_->encoderCtx->channels;
#endif
    audioTranscoder_->resampleFrame->sample_rate = audioTranscoder_->encoderCtx->sample_rate;
    audioTranscoder_->resampleFrame->nb_samples = outSamples;

    ret = av_frame_get_buffer(audioTranscoder_->resampleFrame, 0);
    if (ret < 0) {
        LOG_ERROR("Failed to allocate resample buffer: {}", utils::avErr2Str(ret));
        av_frame_free(&frame);
        return false;
    }

    int converted = swr_convert(audioTranscoder_->swrCtx,
                                 audioTranscoder_->resampleFrame->extended_data, outSamples,
                                 (const uint8_t **)frame->extended_data, frame->nb_samples);
    if (converted < 0) {
        LOG_ERROR("Failed to resample: {}", utils::avErr2Str(converted));
        av_frame_free(&frame);
        return false;
    }
    audioTranscoder_->resampleFrame->nb_samples = converted;

    // 写入 FIFO
    int written = av_audio_fifo_write(audioTranscoder_->audioFifo,
                                      (void **)audioTranscoder_->resampleFrame->extended_data,
                                      audioTranscoder_->resampleFrame->nb_samples);
    if (written < audioTranscoder_->resampleFrame->nb_samples) {
        LOG_WARN("Audio FIFO overflow or write error");
    }

    av_frame_free(&frame);

    // 从 FIFO 读取并编码
    while (av_audio_fifo_size(audioTranscoder_->audioFifo) >= audioTranscoder_->encoderCtx->frame_size) {
        ret = av_audio_fifo_read(audioTranscoder_->audioFifo,
                                 (void **)audioTranscoder_->fifoFrame->extended_data,
                                 audioTranscoder_->encoderCtx->frame_size);
        if (ret < audioTranscoder_->encoderCtx->frame_size) {
            LOG_ERROR("Failed to read from FIFO");
            break;
        }

        audioTranscoder_->fifoFrame->pts = audioTranscoder_->pts;
        audioTranscoder_->pts += audioTranscoder_->fifoFrame->nb_samples;

        ret = avcodec_send_frame(audioTranscoder_->encoderCtx, audioTranscoder_->fifoFrame);
        if (ret < 0) {
            LOG_ERROR("Failed to send frame to encoder: {}", utils::avErr2Str(ret));
            break;
        }

        // 接收编码后的数据包
        AVPacket *outPacket = av_packet_alloc();
        if (!outPacket) {
            break;
        }

        ret = avcodec_receive_packet(audioTranscoder_->encoderCtx, outPacket);
        if (ret == 0) {
            // 调整时间戳
            outPacket->stream_index = outStream->index;
            outPacket->pts = av_rescale_q(outPacket->pts,
                                          audioTranscoder_->encoderCtx->time_base,
                                          outStream->time_base);
            outPacket->dts = av_rescale_q(outPacket->dts,
                                          audioTranscoder_->encoderCtx->time_base,
                                          outStream->time_base);
            outPacket->duration = av_rescale_q(outPacket->duration,
                                               audioTranscoder_->encoderCtx->time_base,
                                               outStream->time_base);

            // 写入输出文件
            ret = av_interleaved_write_frame(outputFormatCtx_, outPacket);
            if (ret < 0) {
                LOG_ERROR("Failed to write audio frame: {}", utils::avErr2Str(ret));
            }
        }
        av_packet_free(&outPacket);

        if (ret == AVERROR(EAGAIN)) {
            break;
        }
    }

    return true;
}

void RealTimeStreamRecorder::flushAudioTranscoder()
{
    if (!audioTranscoder_ || !audioTranscoder_->initialized) {
        return;
    }

    // 刷新解码器
    avcodec_send_packet(audioTranscoder_->decoderCtx, nullptr);

    // 接收剩余的解码帧
    AVFrame *frame = av_frame_alloc();
    if (frame) {
        int ret;
        while ((ret = avcodec_receive_frame(audioTranscoder_->decoderCtx, frame)) == 0) {
            // 重采样并写入 FIFO
            int64_t delay = swr_get_delay(audioTranscoder_->swrCtx, frame->sample_rate);
            int64_t outSamples = av_rescale_rnd(delay + frame->nb_samples,
                                                audioTranscoder_->encoderCtx->sample_rate,
                                                frame->sample_rate, AV_ROUND_UP);

            av_frame_unref(audioTranscoder_->resampleFrame);
            audioTranscoder_->resampleFrame->format = audioTranscoder_->encoderCtx->sample_fmt;
#if LIBAVCODEC_VERSION_MAJOR >= 60
            av_channel_layout_copy(&audioTranscoder_->resampleFrame->ch_layout,
                                   &audioTranscoder_->encoderCtx->ch_layout);
#else
            audioTranscoder_->resampleFrame->channel_layout = audioTranscoder_->encoderCtx->channel_layout;
            audioTranscoder_->resampleFrame->channels = audioTranscoder_->encoderCtx->channels;
#endif
            audioTranscoder_->resampleFrame->sample_rate = audioTranscoder_->encoderCtx->sample_rate;
            audioTranscoder_->resampleFrame->nb_samples = outSamples;

            if (av_frame_get_buffer(audioTranscoder_->resampleFrame, 0) >= 0) {
                int converted = swr_convert(audioTranscoder_->swrCtx,
                                             audioTranscoder_->resampleFrame->extended_data,
                                             outSamples,
                                             (const uint8_t **)frame->extended_data,
                                             frame->nb_samples);
                if (converted > 0) {
                    audioTranscoder_->resampleFrame->nb_samples = converted;
                    av_audio_fifo_write(audioTranscoder_->audioFifo,
                                        (void **)audioTranscoder_->resampleFrame->extended_data,
                                        audioTranscoder_->resampleFrame->nb_samples);
                }
            }
        }
        av_frame_free(&frame);
    }

    // 刷新编码器
    avcodec_send_frame(audioTranscoder_->encoderCtx, nullptr);

    // 接收剩余的编码数据包
    AVPacket *outPacket = av_packet_alloc();
    if (outPacket) {
        int ret;
        while ((ret = avcodec_receive_packet(audioTranscoder_->encoderCtx, outPacket)) == 0) {
            outPacket->stream_index = 0;  // 音频流索引
            av_interleaved_write_frame(outputFormatCtx_, outPacket);
        }
        av_packet_free(&outPacket);
    }
}

void RealTimeStreamRecorder::cleanupAudioTranscoder()
{
    if (!audioTranscoder_) {
        return;
    }

    if (audioTranscoder_->decoderCtx) {
        avcodec_free_context(&audioTranscoder_->decoderCtx);
    }
    if (audioTranscoder_->encoderCtx) {
        avcodec_free_context(&audioTranscoder_->encoderCtx);
    }
    if (audioTranscoder_->swrCtx) {
        swr_free(&audioTranscoder_->swrCtx);
    }
    if (audioTranscoder_->audioFifo) {
        av_audio_fifo_free(audioTranscoder_->audioFifo);
    }
    if (audioTranscoder_->resampleFrame) {
        av_frame_free(&audioTranscoder_->resampleFrame);
    }
    if (audioTranscoder_->fifoFrame) {
        av_frame_free(&audioTranscoder_->fifoFrame);
    }

    audioTranscoder_.reset();
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END