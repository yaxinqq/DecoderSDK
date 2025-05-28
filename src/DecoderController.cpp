#include "DecoderController.h"
#include "Logger.h"
#include "Utils.h"

DecoderController::DecoderController()
    : demuxer_(std::make_shared<Demuxer>()),
      syncController_(std::make_shared<SyncController>())
{
}

DecoderController::~DecoderController()
{
    stopDecode();
    close();
}

bool DecoderController::open(const std::string& filePath, const Config& config)
{
    config_ = config;
    isLiveStream_ = utils::isRealtime(filePath);

    // 打开媒体文件
    if (!demuxer_->open(filePath, isLiveStream_)) {
        return false;
    }

    // 启动解复用器
    demuxer_->start();
    return true;
}

bool DecoderController::close()
{
    // 如果在录像，则先停止
    if (isRecording()) {
        stopRecording();
    }

    // 停止解复用器
    if (demuxer_) {
        demuxer_->stop();
        demuxer_->close();
    }

    return true;
}

bool DecoderController::pause()
{
    if (!demuxer_) {
        return false;
    }

    return demuxer_->pause();
}

bool DecoderController::resume()
{
    if (!demuxer_) {
        return false;
    }

    return demuxer_->resume();
}

bool DecoderController::startDecode()
{
    // 如果当前已开始解码，则先停止
    if (isStartDecoding_) {
        stopDecode();
    }

    // 重置同步控制器
    syncController_->resetClocks();

    // 创建视频解码器
    if (demuxer_->hasVideo()) {
        videoDecoder_ =
            std::make_shared<VideoDecoder>(demuxer_, syncController_);
        videoDecoder_->init(config_.hwAccelType, config_.hwDeviceIndex,
                            config_.videoOutFormat);
        videoDecoder_->setFrameRateControl(config_.enableFrameRateControl);
        videoDecoder_->setSpeed(config_.speed);
        if (!videoDecoder_->open()) {
            return false;
        }
    }

    // 创建音频解码器
    if (demuxer_->hasAudio()) {
        audioDecoder_ =
            std::make_shared<AudioDecoder>(demuxer_, syncController_);
        audioDecoder_->setSpeed(config_.speed);
        if (!audioDecoder_->open()) {
            return false;
        }
    }

    // 默认使用音频作为主时钟
    if (demuxer_->hasAudio()) {
        syncController_->setMaster(SyncController::MasterClock::Audio);
    } else if (demuxer_->hasVideo()) {
        syncController_->setMaster(SyncController::MasterClock::Video);
    }

    // 启动解码器
    if (videoDecoder_) {
        videoDecoder_->start();
    }

    if (audioDecoder_) {
        audioDecoder_->start();
    }

    isStartDecoding_ = true;
    return true;
}

bool DecoderController::stopDecode()
{
    // 停止解码器，并销毁
    if (videoDecoder_) {
        videoDecoder_->stop();
        videoDecoder_.reset();
    }

    if (audioDecoder_) {
        audioDecoder_->stop();
        audioDecoder_.reset();
    }

    isStartDecoding_ = false;
    return true;
}

bool DecoderController::seek(double position)
{
    if (!demuxer_) {
        return false;
    }

    // 如果是实时流，则不支持seek
    if (isLiveStream_) {
        return false;
    }

    // 暂停解码器
    bool wasPaused = false;
    if (videoDecoder_ || audioDecoder_) {
        wasPaused = demuxer_->isPaused();
        if (!wasPaused) {
            demuxer_->pause();
        }
    }

    // 考虑倍速因素调整seek位置
    // 注意：这里不需要调整position，因为position是目标时间点，与倍速无关

    // 执行seek操作
    bool result = demuxer_->seek(position);

    if (result) {
        // 清空队列，并设置seek节点
        if (videoDecoder_) {
            videoDecoder_->setSeekPos(position);
        }
        if (audioDecoder_) {
            audioDecoder_->setSeekPos(position);
        }

        // 重置同步控制器的时钟
        syncController_->resetClocks();
    }

    // 如果之前没有暂停，则恢复播放
    if (!wasPaused) {
        demuxer_->resume();
    }

    return result;
}

bool DecoderController::setSpeed(double speed)
{
    if (speed <= 0.0f) {
        return false;
    }

    // 如果是实时流，则不支持设置速度
    if (isLiveStream_) {
        return false;
    }

    config_.speed = speed;

    // 设置解码器速度
    if (videoDecoder_) {
        videoDecoder_->setSpeed(speed);
    }
    if (audioDecoder_) {
        audioDecoder_->setSpeed(speed);
    }

    // 设置时钟速度
    if (syncController_) {
        syncController_->setSpeed(speed);
    }

    return true;
}

FrameQueue& DecoderController::videoQueue()
{
    static FrameQueue emptyQueue;
    return videoDecoder_ ? videoDecoder_->frameQueue() : emptyQueue;
}

FrameQueue& DecoderController::audioQueue()
{
    static FrameQueue emptyQueue;
    return audioDecoder_ ? audioDecoder_->frameQueue() : emptyQueue;
}

void DecoderController::setMasterClock(SyncController::MasterClock type)
{
    syncController_->setMaster(type);
}

double DecoderController::getVideoFrameRate() const
{
    return videoDecoder_ ? videoDecoder_->getFrameRate() : 0.0;
}

void DecoderController::setFrameRateControl(bool enable)
{
    if (videoDecoder_) {
        videoDecoder_->setFrameRateControl(enable);
    }
    config_.enableFrameRateControl = enable;
}

bool DecoderController::isFrameRateControlEnabled() const
{
    return videoDecoder_ ? videoDecoder_->isFrameRateControlEnabled() : false;
}

double DecoderController::curSpeed() const
{
    return config_.speed;
}

bool DecoderController::startRecording(const std::string& outputPath)
{
    std::lock_guard<std::mutex> lock(recordMutex_);

    // 如果正在录像，则返回
    if (isRecording_.load()) {
        return false;
    }

    // 如果没有打开媒体文件，则返回
    if (!demuxer_ || !demuxer_->formatContext()) {
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
    AVFormatContext* inputFormatCtx = demuxer_->formatContext();
    for (unsigned int i = 0; i < inputFormatCtx->nb_streams; i++) {
        AVStream* inStream = inputFormatCtx->streams[i];
        if (inStream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            inStream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }

        // 创建新的输出流
        AVStream* outStream = avformat_new_stream(recordFormatCtx_, nullptr);
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
    AVDictionary* opts = nullptr;
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
    demuxer_->initRecordQueue();

    // 启动录像线程
    isRecording_.store(true);
    recordStopFlag_.store(false);
    recordThread_ = std::thread(&DecoderController::recordingLoop, this);

    return true;
}

bool DecoderController::stopRecording()
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
    demuxer_->destroyRecordQueue();

    // 判断recordFormatCtx_是否为空，如果为空，则直接返回
    if (!recordFormatCtx_) {
        LOG_WARN("recordFormatCtx_ is nullptr, file {} record failed!",
                 recordFilePath_);
        isRecording_.store(false);
        return true;
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

    // 重置录像状态
    isRecording_.store(false);
    recordFilePath_.clear();

    return true;
}

bool DecoderController::isRecording() const
{
    return isRecording_.load();
}

void DecoderController::recordingLoop()
{
    while (!demuxer_ || !recordFormatCtx_) {
        return;
    }

    AVFormatContext* inputFormatCtx = demuxer_->formatContext();

    // 创建流索引映射表
    int* streamMapping =
        (int*)av_malloc_array(inputFormatCtx->nb_streams, sizeof(int));
    if (!streamMapping) {
        LOG_ERROR(
            "Failed to allocate stream mapping array, file {} record failed!",
            recordFilePath_);
        return;
    }

    // 初始化流映射
    int streamIndex = 0;
    for (unsigned int i = 0; i < inputFormatCtx->nb_streams; i++) {
        AVStream* inStream = inputFormatCtx->streams[i];
        if (inStream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            inStream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            streamMapping[i] = -1;
            continue;
        }
        streamMapping[i] = streamIndex++;
    }

    // 创建RecordPacketQueue的引用，取包
    auto videoQueue = demuxer_->recordPacketQueue(AVMEDIA_TYPE_VIDEO);
    auto audioQueue = demuxer_->recordPacketQueue(AVMEDIA_TYPE_AUDIO);

    // 记录已处理的packet序列号。这里先不管序列号，也不管是否seek，只是单纯的记录
    int videoSerial = videoQueue ? videoQueue->serial() : -1;
    int audioSerial = audioQueue ? audioQueue->serial() : -1;

    // 创建临时packet，从队列中获取数据
    AVPacket* tempPacket = av_packet_alloc();
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
                    AVStream* inStream =
                        inputFormatCtx->streams[tempPacket->stream_index];
                    AVStream* outStream =
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
                    AVStream* inStream =
                        inputFormatCtx->streams[packet.get()->stream_index];
                    AVStream* outStream =
                        recordFormatCtx_->streams[outStreamIndex];
                    av_packet_rescale_ts(tempPacket, inStream->time_base,
                                         outStream->time_base);

                    // 写入packet
                    //std::lock_guard<std::mutex> lock(recordMutex_);
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