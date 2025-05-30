#include "Demuxer.h"
#include "EventDispatcher.h"
#include "Logger.h"

namespace {
constexpr int kReadErrorMaxCount = 5;
}

Demuxer::Demuxer(std::shared_ptr<EventDispatcher> eventDispatcher)
    : eventDispatcher_(eventDispatcher)
{
    videoPacketQueue_ = std::make_shared<PacketQueue>(INT_MAX);
    audioPacketQueue_ = std::make_shared<PacketQueue>(INT_MAX);
}

Demuxer::~Demuxer()
{
    close();
}

bool Demuxer::open(const std::string &url, bool isRealTime, bool isReopen)
{
    // 上报流正在打开事件
    auto event = std::make_shared<StreamEventArgs>(url, "DecoderController",
                                                   "Stream Opening");
    eventDispatcher_->triggerEventAsync(EventType::kStreamOpening, event);

    const auto sendFailedEvent = [this, isReopen, url]() {
        // 上报流打开失败事件
        auto event = std::make_shared<StreamEventArgs>(url, "Demuxer",
                                                       "Stream OpenFiled");
        eventDispatcher_->triggerEventAsync(EventType::kStreamOpenFailed,
                                            event);
    };

    AVDictionary *options = nullptr;
    av_dict_set(&options, "timeout", "3000000", 0);
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "max_delay", "0.0", 0);
    av_dict_set(&options, "buffer_size", "1048576", 0);  // 1MB

    if (isRealTime)
        av_dict_set(&options, "fflags", "nobuffer", 0);

    if (avformat_open_input(&formatContext_, url.c_str(), nullptr, &options) !=
        0) {
        av_dict_free(&options);

        sendFailedEvent();
        return false;
    }
    av_dict_free(&options);

    if (avformat_find_stream_info(formatContext_, nullptr) < 0) {
        sendFailedEvent();
        return false;
    }

    int ret = av_find_best_stream(formatContext_, AVMEDIA_TYPE_VIDEO, -1, -1,
                                  nullptr, 0);
    videoStreamIndex_ = ret >= 0 ? ret : -1;

    ret = av_find_best_stream(formatContext_, AVMEDIA_TYPE_AUDIO, -1, -1,
                              nullptr, 0);
    audioStreamIndex_ = ret >= 0 ? ret : -1;

    // 开启解复用器
    start();

    // 上报流打开成功事件
    event = std::make_shared<StreamEventArgs>(url, "DecoderController",
                                              "Stream Opened");
    eventDispatcher_->triggerEventAsync(EventType::kStreamOpened, event);

    url_ = url;
    isRealTime_ = isRealTime;
    needClose_ = true;
    isReopen_ = isReopen;
    if (isReopen) {
        // 上报流恢复事件
        event = std::make_shared<StreamEventArgs>(url, "DecoderController",
                                                  "Stream Recovery");
        eventDispatcher_->triggerEventAsync(EventType::kStreamReadRecovery,
                                            event);
    }

    return true;
}

bool Demuxer::close()
{
    if (!needClose_) {
        return true;
    }

    // 上报流关闭事件
    auto event =
        std::make_shared<StreamEventArgs>(url_, "Demuxer", "Stream Close");
    eventDispatcher_->triggerEventAsync(EventType::kStreamClose, event);

    // 如果在录像，则先停止
    if (isRecording()) {
        stopRecording();
    }

    // 停止解复用线程
    stop();

    // 销毁上下文
    if (formatContext_) {
        avformat_close_input(&formatContext_);
        formatContext_ = nullptr;
    }

    // 上报流已关闭事件
    event = std::make_shared<StreamEventArgs>(url_, "Demuxer", "Stream Closed");
    eventDispatcher_->triggerEventAsync(EventType::kStreamClosed, event);

    needClose_ = false;
    isReopen_ = false;

    url_.clear();
    videoStreamIndex_ = -1;
    audioStreamIndex_ = -1;
    return true;
}

bool Demuxer::pause()
{
    if (!isRunning_.load()) {
        return false;
    }

    isPaused_.store(true);
    return true;
}

bool Demuxer::resume()
{
    if (!isRunning_.load()) {
        return false;
    }

    isPaused_.store(false);
    return true;
}

bool Demuxer::seek(double position)
{
    if (!formatContext_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    int streamIndex =
        videoStreamIndex_ >= 0 ? videoStreamIndex_ : audioStreamIndex_;
    if (streamIndex < 0) {
        return false;
    }

    AVStream *stream = formatContext_->streams[streamIndex];
    if (!stream) {
        return false;
    }

    int64_t seekPos =
        av_rescale_q((int64_t)(position * AV_TIME_BASE),
                     AVRational{1, AV_TIME_BASE}, stream->time_base);
    int ret = avformat_seek_file(formatContext_, streamIndex, INT64_MIN,
                                 seekPos, INT64_MAX, 0);
    if (ret < 0) {
        return false;
    }

    // 清空队列
    if (videoPacketQueue_) {
        videoPacketQueue_->flush();
    }
    if (audioPacketQueue_) {
        audioPacketQueue_->flush();
    }

    return true;
}

AVFormatContext *Demuxer::formatContext() const
{
    return formatContext_;
}

int Demuxer::streamIndex(AVMediaType mediaType) const
{
    switch (mediaType) {
        case AVMEDIA_TYPE_VIDEO:
            return videoStreamIndex_;
        case AVMEDIA_TYPE_AUDIO:
            return audioStreamIndex_;
        default:
            return -1;
    }
}

std::shared_ptr<PacketQueue> Demuxer::packetQueue(AVMediaType mediaType) const
{
    switch (mediaType) {
        case AVMEDIA_TYPE_VIDEO:
            return videoPacketQueue_;
        case AVMEDIA_TYPE_AUDIO:
            return audioPacketQueue_;
        default:
            return nullptr;
    }
}

bool Demuxer::hasVideo() const
{
    return videoStreamIndex_ >= 0 && formatContext_ != nullptr;
}

bool Demuxer::hasAudio() const
{
    return audioStreamIndex_ >= 0 && formatContext_ != nullptr;
}

bool Demuxer::isPaused() const
{
    return isPaused_.load();
}

bool Demuxer::isRealTime() const
{
    return isRealTime_;
}

std::string Demuxer::url() const
{
    return url_;
}

// 开始录像，暂时将输出文件保存为.mp4格式
bool Demuxer::startRecording(const std::string &outputPath)
{
    std::lock_guard<std::mutex> lock(recordMutex_);

    // 如果正在录像，则返回
    if (isRecording_.load()) {
        return false;
    }

    // 如果没有打开媒体文件，则返回
    if (!formatContext_) {
        return false;
    }

    // 创建输出格式上下文
    avformat_alloc_output_context2(&recordFormatCtx_, nullptr, "mp4",
                                   outputPath.c_str());
    if (!recordFormatCtx_) {
        LOG_ERROR("Failed to allocate output format context");
        return false;
    }

    // 复制媒体文件的流信息到输出格式上下文
    AVFormatContext *inputFormatCtx = formatContext_;
    for (unsigned int i = 0; i < inputFormatCtx->nb_streams; i++) {
        AVStream *inStream = inputFormatCtx->streams[i];
        if (inStream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            inStream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }

        // 创建新的输出流
        AVStream *outStream = avformat_new_stream(recordFormatCtx_, nullptr);
        if (!outStream) {
            LOG_ERROR("Failed to allocate output stream");
            avformat_free_context(recordFormatCtx_);
            recordFormatCtx_ = nullptr;
            return false;
        }

        // 复制解码器参数
        if (avcodec_parameters_copy(outStream->codecpar, inStream->codecpar) <
            0) {
            LOG_ERROR("Failed to copy codec parameters");
            avformat_free_context(recordFormatCtx_);
            recordFormatCtx_ = nullptr;
            return false;
        }

        // 确保不设置编解码器标志
        outStream->codecpar->codec_tag = 0;
    }

    // 打开输出文件
    if (!(recordFormatCtx_->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&recordFormatCtx_->pb, outputPath.c_str(),
                      AVIO_FLAG_WRITE) < 0) {
            LOG_ERROR("Failed to open output file: {}", outputPath);
            avformat_free_context(recordFormatCtx_);
            recordFormatCtx_ = nullptr;
            return false;
        }
    }

    // 写入文件头
    AVDictionary *opts = nullptr;
    if (avformat_write_header(recordFormatCtx_, &opts) < 0) {
        if (!(recordFormatCtx_->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&recordFormatCtx_->pb);
        }

        LOG_ERROR("Failed to write header to output file: {}", outputPath);
        avformat_free_context(recordFormatCtx_);
        recordFormatCtx_ = nullptr;
        return false;
    }

    // 保存输出路径
    recordFilePath_ = outputPath;

    // 初始化录像队列
    initRecordQueue();

    // 启动录像线程
    isRecording_.store(true);
    recordStopFlag_.store(false);
    recordThread_ = std::thread(&Demuxer::recordingLoop, this);

    // 发送开始录制的事件
    auto event = std::make_shared<RecordingEventArgs>(
        recordFilePath_, "mp4", "Demuxer", "Recording Started");
    eventDispatcher_->triggerEventAsync(EventType::kRecordingStarted, event);

    return true;
}

bool Demuxer::stopRecording()
{
    std::lock_guard<std::mutex> lock(recordMutex_);

    // 如果没有正在录像，则返回
    if (!isRecording_.load()) {
        return false;
    }

    // 设置停止标志，并等待线程结束
    recordStopFlag_.store(true);
    recordCv_.notify_all();
    if (recordThread_.joinable()) {
        recordThread_.join();
    }

    // 销毁录像队列
    destroyRecordQueue();

    // 判断recordFormatCtx_是否为空，如果为空，则直接返回
    if (!recordFormatCtx_) {
        LOG_WARN("recordFormatCtx_ is nullptr, file {} record failed!",
                 recordFilePath_);
        isRecording_.store(false);

        // 发送录制错误的事件
        auto event = std::make_shared<RecordingEventArgs>(
            recordFilePath_, "mp4", "Demuxer", "Recording Error");
        eventDispatcher_->triggerEventAsync(EventType::kRecordingError, event);

        return false;
    }

    // 写入文件尾
    av_write_trailer(recordFormatCtx_);

    // 关闭输出文件
    if (!(recordFormatCtx_->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&recordFormatCtx_->pb);
    }

    // 释放输出格式上下文
    avformat_free_context(recordFormatCtx_);
    recordFormatCtx_ = nullptr;

    // 发送结束录制的事件
    auto event = std::make_shared<RecordingEventArgs>(
        recordFilePath_, "mp4", "Demuxer", "Recording Stopped");
    eventDispatcher_->triggerEventAsync(EventType::kRecordingStopped, event);

    // 重置录像状态
    isRecording_.store(false);
    recordFilePath_.clear();

    return true;
}

bool Demuxer::isRecording() const
{
    return isRecording_.load();
}

void Demuxer::demuxLoop()
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return;

    int errorTimes = 0;
    bool reopened = false;
    bool readFirstPacket = false;

    while (isRunning_.load()) {
        // 检查是否暂停
        if (isPaused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // 检查队列是否已满，如果已满则等待
        if ((videoStreamIndex_ >= 0 && videoPacketQueue_->isFull()) ||
            (audioStreamIndex_ >= 0 && audioPacketQueue_->isFull())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        int ret = av_read_frame(formatContext_, pkt);
        if (ret < 0) {
            // 文件结束或错误
            if (ret == AVERROR_EOF || avio_feof(formatContext_->pb)) {
                // 文件结束，发送空包表示结束
                av_packet_unref(pkt);

                if (videoStreamIndex_ >= 0) {
                    pkt->stream_index = videoStreamIndex_;
                    Packet endPacket(pkt);
                    endPacket.setSerial(videoPacketQueue_->serial());
                    videoPacketQueue_->push(endPacket);

                    // 如果存在录像队列，则也发送空包表示结束
                    if (videoRecordPacketQueue_) {
                        videoRecordPacketQueue_->push(endPacket);
                    }
                }

                if (audioStreamIndex_ >= 0) {
                    pkt->stream_index = audioStreamIndex_;
                    Packet endPacket(pkt);
                    endPacket.setSerial(audioPacketQueue_->serial());
                    audioPacketQueue_->push(endPacket);

                    // 如果存在录像队列，则也发送空包表示结束
                    if (audioRecordPacketQueue_) {
                        audioRecordPacketQueue_->push(endPacket);
                    }
                }

                // 发送流结束事件
                auto event = std::make_shared<StreamEventArgs>(url_, "Demuxer",
                                                               "Stream Ended");
                eventDispatcher_->triggerEventAsync(EventType::kStreamEnded,
                                                    event);

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else if (ret != AVERROR(EAGAIN)) {
                // 真正的错误
                if (errorTimes++ >= kReadErrorMaxCount) {
                    // 连续多次读取错误，启动重连机制（多见于实时流）
                    LOG_ERROR(
                        "Read frame failed, error times: {}, more than {}, "
                        "reinit!",
                        errorTimes, kReadErrorMaxCount);

                    // 上报流错误事件
                    auto event = std::make_shared<StreamEventArgs>(
                        url_, "Demuxer", "Stream Read Error");
                    eventDispatcher_->triggerEventAsync(
                        EventType::kStreamReadError, event);

                    break;
                }
            }
            continue;
        }
        // 读出正确的包后，错误次数清零
        errorTimes = 0;

        // 当从reopened恢复时，发送流恢复事件
        if (reopened) {
            auto event = std::make_shared<StreamEventArgs>(url_, "Demuxer",
                                                           "Stream Recovered");
            eventDispatcher_->triggerEventAsync(EventType::kStreamReadRecovery,
                                                event);
            reopened = false;
        }

        // 读到第一个包后，发送事件
        if (!readFirstPacket) {
            readFirstPacket = true;

            auto event = std::make_shared<StreamEventArgs>(url_, "Demuxer",
                                                           "Stream Read Data");
            eventDispatcher_->triggerEventAsync(EventType::kStreamReadData,
                                                event);
        }

        // 根据流类型分发数据包
        if (pkt->stream_index == videoStreamIndex_) {
            Packet packet(pkt);
            packet.setSerial(videoPacketQueue_->serial());
            videoPacketQueue_->push(packet);

            // 如果存在录像队列，则也将数据包放入录像队列
            if (videoRecordPacketQueue_) {
                videoRecordPacketQueue_->push(packet);
            }
        } else if (pkt->stream_index == audioStreamIndex_) {
            Packet packet(pkt);
            packet.setSerial(audioPacketQueue_->serial());
            audioPacketQueue_->push(packet);

            // 如果存在录像队列，则也将数据包放入录像队列
            if (audioRecordPacketQueue_) {
                audioRecordPacketQueue_->push(packet);
            }
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
}

void Demuxer::recordingLoop()
{
    if (!recordFormatCtx_) {
        return;
    }

    AVFormatContext *inputFormatCtx = formatContext_;

    // 创建流索引映射表
    int *streamMapping =
        (int *)av_malloc_array(inputFormatCtx->nb_streams, sizeof(int));
    if (!streamMapping) {
        LOG_ERROR(
            "Failed to allocate stream mapping array, file {} record failed!",
            recordFilePath_);
        return;
    }

    // 初始化流映射
    int streamIndex = 0;
    for (unsigned int i = 0; i < inputFormatCtx->nb_streams; i++) {
        AVStream *inStream = inputFormatCtx->streams[i];
        if (inStream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            inStream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            streamMapping[i] = -1;
            continue;
        }
        streamMapping[i] = streamIndex++;
    }

    // 创建RecordPacketQueue的引用，取包
    auto videoQueue = recordPacketQueue(AVMEDIA_TYPE_VIDEO);
    auto audioQueue = recordPacketQueue(AVMEDIA_TYPE_AUDIO);

    // 记录已处理的packet序列号。这里先不管序列号，也不管是否seek，只是单纯的记录
    int videoSerial = videoQueue ? videoQueue->serial() : -1;
    int audioSerial = audioQueue ? audioQueue->serial() : -1;

    // 创建临时packet，从队列中获取数据
    AVPacket *tempPacket = av_packet_alloc();
    if (!tempPacket) {
        LOG_ERROR("Failed to allocate temporary packet, file {} record failed!",
                  recordFilePath_);
        av_free(streamMapping);
        return;
    }

    // 是否得到了关键帧
    bool hasKeyFrame = false;

    // 处理循环
    while (!recordStopFlag_.load()) {
        // 检查视频队列
        if (videoQueue && videoQueue->serial() != videoSerial) {
            // 序列号变化，说明发生了seek操作，需要写入新的文件头
            // 这里简化处理，实际可能需要创建新文件
            videoSerial = videoQueue->serial();
            hasKeyFrame = false;
        }

        // 检查音频队列
        if (audioQueue && audioQueue->serial() != audioSerial) {
            // 序列号变化，说明发生了seek操作
            audioSerial = audioQueue->serial();
        }

        // 从视频和音频队列中获取packet并写入文件
        // 注意：这里的实现是简化的，实际需要考虑队列同步和时间戳调整

        // 处理视频packet
        Packet packet;
        bool gotPacket = false;

        // 尝试从视频队列获取Packet
        if (videoQueue) {
            if (videoQueue->pop(packet, 1)) {
                gotPacket = true;

                // 复制packet数据
                av_packet_ref(tempPacket, packet.get());

                // 如果当前没有关键帧，且这个视频帧也不是关键帧，就跳过
                if (!hasKeyFrame &&
                    (tempPacket->flags & AV_PKT_FLAG_KEY) == 0) {
                    continue;
                }
                hasKeyFrame = true;

                // 调整stream_index到输出文件的索引
                int outStreamIndex = streamMapping[tempPacket->stream_index];
                if (outStreamIndex >= 0) {
                    tempPacket->stream_index = outStreamIndex;

                    // 调整时间戳
                    AVStream *inStream =
                        inputFormatCtx->streams[tempPacket->stream_index];
                    AVStream *outStream =
                        recordFormatCtx_->streams[outStreamIndex];
                    av_packet_rescale_ts(tempPacket, inStream->time_base,
                                         outStream->time_base);

                    // 写入packet
                    /*std::lock_guard<std::mutex> lock(recordMutex_);*/
                    if (av_interleaved_write_frame(recordFormatCtx_,
                                                   tempPacket) < 0) {
                        LOG_ERROR("Failed to write frame to output file: {}",
                                  recordFilePath_);
                    }
                }

                av_packet_unref(tempPacket);
            }
        }

        // 尝试从音频队列获取packet
        if (audioQueue && hasKeyFrame) {
            if (audioQueue->pop(packet, 1)) {
                // 复制packet数据
                av_packet_ref(tempPacket, packet.get());

                // 调整stream_index到输出文件的索引
                int outStreamIndex = streamMapping[tempPacket->stream_index];
                if (outStreamIndex >= 0) {
                    tempPacket->stream_index = outStreamIndex;

                    // 调整时间戳
                    AVStream *inStream =
                        inputFormatCtx->streams[packet.get()->stream_index];
                    AVStream *outStream =
                        recordFormatCtx_->streams[outStreamIndex];
                    av_packet_rescale_ts(tempPacket, inStream->time_base,
                                         outStream->time_base);

                    // 写入packet
                    // std::lock_guard<std::mutex> lock(recordMutex_);
                    if (av_interleaved_write_frame(recordFormatCtx_,
                                                   tempPacket) < 0) {
                        // 写入失败处理
                        LOG_ERROR("Failed to write frame to output file: {}",
                                  recordFilePath_);
                    }
                }

                av_packet_unref(tempPacket);
            }
        }
    }

    // 释放资源
    av_free(streamMapping);
    av_packet_free(&tempPacket);
}

void Demuxer::start()
{
    if (isRunning_.load()) {
        // Todo: log
        return;
    }

    // 启动队列
    videoPacketQueue_->start();
    audioPacketQueue_->start();

    isRunning_.store(true);
    thread_ = std::thread(&Demuxer::demuxLoop, this);

    return;
}

void Demuxer::stop()
{
    if (!isRunning_.load()) {
        // Todo: log
        return;
    }

    isRunning_.store(false);

    // 中止队列
    videoPacketQueue_->abort();
    audioPacketQueue_->abort();

    // 销毁录像队列（如果存在）
    destroyRecordQueue();

    if (thread_.joinable()) {
        thread_.join();
    }

    return;
}

void Demuxer::initRecordQueue()
{
    videoRecordPacketQueue_.reset(new PacketQueue(INT_MAX));
    audioRecordPacketQueue_.reset(new PacketQueue(INT_MAX));
}

void Demuxer::destroyRecordQueue()
{
    if (videoPacketQueue_)
        videoRecordPacketQueue_.reset();

    if (audioPacketQueue_)
        audioRecordPacketQueue_.reset();
}

std::shared_ptr<PacketQueue> Demuxer::recordPacketQueue(
    AVMediaType mediaType) const
{
    switch (mediaType) {
        case AVMEDIA_TYPE_VIDEO:
            return videoRecordPacketQueue_;
        case AVMEDIA_TYPE_AUDIO:
            return audioRecordPacketQueue_;
        default:
            return nullptr;
    }
}