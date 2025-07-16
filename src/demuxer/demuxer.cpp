#include "demuxer.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/time.h>
}

#include <optional>

#include "event_system/event_dispatcher.h"
#include "logger/logger.h"
#include "utils/common_utils.h"

namespace {
constexpr int kReadErrorMaxCount = 25;
}

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

Demuxer::Demuxer(std::shared_ptr<EventDispatcher> eventDispatcher)
    : eventDispatcher_(eventDispatcher),
      realTimeStreamRecorder_(std::make_unique<RealTimeStreamRecorder>(eventDispatcher)),
      loopMode_(LoopMode::kNone),
      maxLoops_(-1),
      currentLoopCount_(0)
{
}

Demuxer::~Demuxer()
{
    close();
}

bool Demuxer::open(const std::string &url, bool isRealTime, Config::DecodeMediaType decodeMediaType,
                   bool isReopen)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 上报流正在打开事件
    auto event = std::make_shared<StreamEventArgs>(url, "Demuxer", "Stream Opening");
    eventDispatcher_->triggerEvent(EventType::kStreamOpening, event);

    const auto sendFailedEvent = [this, url]() {
        auto event = std::make_shared<StreamEventArgs>(url, "Demuxer", "Stream Open Failed");
        eventDispatcher_->triggerEvent(EventType::kStreamOpenFailed, event);
    };

    // 设置FFmpeg选项
    AVDictionary *options = nullptr;
    av_dict_set(&options, "timeout", "2000000", 0); // 2秒超时
    av_dict_set(&options, "max_delay", "100000", 0);
    av_dict_set(&options, "buffer_size", "10000000", 0); // 1MB缓冲
    av_dict_set(&options, "analyzeduration", "1000000", 0);

    if (isRealTime) {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "fflags", "nobuffer", 0);
        av_dict_set(&options, "stimeout", "2000000", 0);
    }

    // 打开输入
    int ret = avformat_open_input(&formatContext_, url.c_str(), nullptr, &options);
    av_dict_free(&options);

    if (ret != 0) {
        LOG_ERROR("Failed to open input: {} - {}", url, utils::avErr2Str(ret));
        sendFailedEvent();
        return false;
    }

    // 查找流信息
    ret = avformat_find_stream_info(formatContext_, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to find stream info: {}", utils::avErr2Str(ret));
        sendFailedEvent();
        avformat_close_input(&formatContext_);
        return false;
    }

    // 重置到文件开始位置
    if (formatContext_->pb && formatContext_->pb->seekable) {
        avio_seek(formatContext_->pb, 0, SEEK_SET);
    }

    // 查找最佳流
    videoStreamIndex_ = av_find_best_stream(formatContext_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audioStreamIndex_ = av_find_best_stream(formatContext_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    // 创建数据包队列
    if (videoStreamIndex_ >= 0 && (decodeMediaType & Config::DecodeMediaType::kVideo)) {
        videoPacketQueue_ = std::make_shared<PacketQueue>(1000);
    }
    if (audioStreamIndex_ >= 0 && (decodeMediaType & Config::DecodeMediaType::kAudio)) {
        audioPacketQueue_ = std::make_shared<PacketQueue>(1000);
    }

    // 设置状态
    url_ = url;
    isRealTime_.store(isRealTime);
    needClose_ = true;
    isReopen_ = isReopen;

    // 启动解复用器
    start();

    // 获取流总时长（以秒为单位）
    std::optional<int> totalTime;
    if (formatContext_ && formatContext_->duration != AV_NOPTS_VALUE) {
        // 将时长从微秒转换为秒
        totalTime = static_cast<int>(formatContext_->duration / AV_TIME_BASE);
    }

    // 上报成功事件
    event = std::make_shared<StreamEventArgs>(url, "Demuxer", "Stream Opened");
    event->totalTime = totalTime;
    eventDispatcher_->triggerEvent(EventType::kStreamOpened, event);

    if (isReopen) {
        event = std::make_shared<StreamEventArgs>(url, "Demuxer", "Stream Recovery");
        eventDispatcher_->triggerEvent(EventType::kStreamReadRecovery, event);
    }

    LOG_INFO("Successfully opened: {}", url);
    return true;
}

bool Demuxer::close()
{
    // 如果在录像，则先停止
    if (isRecording()) {
        stopRecording();
    }

    // 停止解复用线程
    stop();

    if (!needClose_) {
        return true;
    }

    // 上报流关闭事件
    const auto event = std::make_shared<StreamEventArgs>(url_, "Demuxer", "Stream Close");
    eventDispatcher_->triggerEvent(EventType::kStreamClose, event);

    std::lock_guard<std::mutex> lock(mutex_);

    // 销毁上下文
    if (formatContext_) {
        avformat_close_input(&formatContext_);
        formatContext_ = nullptr;
    }

    // 重置队列
    videoPacketQueue_.reset();
    audioPacketQueue_.reset();

    // 重置状态
    videoStreamIndex_ = -1;
    audioStreamIndex_ = -1;
    url_.clear();
    isRealTime_.store(false);
    needClose_ = false;
    isReopen_ = false;

    return true;
}

bool Demuxer::pause()
{
    if (isPaused_.load()) {
        return true;
    }

    isPaused_.store(true);
    return true;
}

bool Demuxer::resume()
{
    if (!isPaused_.load()) {
        return true;
    }

    // 如果是实时流，恢复前先清空队列
    if (isRealTime_.load()) {
        if (videoPacketQueue_) {
            videoPacketQueue_->flush();
        }
        if (audioPacketQueue_) {
            audioPacketQueue_->flush();
        }
    }

    isPaused_.store(false);
    pauseCv_.notify_all();
    return true;
}

bool Demuxer::seek(double position)
{
    if (!formatContext_ || isRealTime_.load()) {
        return false;
    }

    // 防止并发seek
    bool expected = false;
    if (!isSeeking_.compare_exchange_strong(expected, true)) {
        LOG_WARN("Seek already in progress, ignoring new seek request");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 转换时间戳
    int64_t timestamp = static_cast<int64_t>(position * AV_TIME_BASE);

    // 执行seek
    int ret = av_seek_frame(formatContext_, -1, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        LOG_ERROR("Seek failed: {}", utils::avErr2Str(ret));
        isSeeking_.store(false); // 重置状态
        return false;
    }

    avformat_flush(formatContext_);

    // 刷新队列
    if (videoPacketQueue_) {
        videoPacketQueue_->flush();
    }
    if (audioPacketQueue_) {
        audioPacketQueue_->flush();
    }

    isSeeking_.store(false); // 重置状态
    LOG_INFO("{} seek to position: {:.2f}s", url_, position);
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
    return videoStreamIndex_ >= 0;
}

bool Demuxer::hasAudio() const
{
    return audioStreamIndex_ >= 0;
}

bool Demuxer::isPaused() const
{
    return isPaused_.load();
}

bool Demuxer::isRealTime() const
{
    return isRealTime_.load();
}

std::string Demuxer::url() const
{
    return url_;
}

bool Demuxer::startRecording(const std::string &outputPath)
{
    if (!isRealTime_.load()) {
        LOG_WARN("Record only support real-time stream");
        return false;
    }

    if (!formatContext_) {
        LOG_ERROR("Cannot start recording: no input format context");
        return false;
    }

    return realTimeStreamRecorder_->startRecording(outputPath, formatContext_);
}

bool Demuxer::stopRecording()
{
    return realTimeStreamRecorder_->stopRecording();
}

bool Demuxer::isRecording() const
{
    return realTimeStreamRecorder_->isRecording();
}

void Demuxer::setPreBufferConfig(int videoFrames, int audioPackets, bool requireBoth,
                                 std::function<void()> onPreBufferReady)
{
    preBufferVideoFrames_ = videoFrames;
    preBufferAudioPackets_ = audioPackets;
    requireBothStreams_ = requireBoth;
    preBufferEnabled_ = (videoFrames > 0 || audioPackets > 0);
    preBufferReady_ = false;
    preBufferReadyCallback_ = onPreBufferReady;

    LOG_INFO("PreBuffer config set: video={}, audio={}, requireBoth={}", videoFrames, audioPackets,
             requireBoth);
}

bool Demuxer::isPreBufferReady() const
{
    return preBufferReady_.load();
}

PreBufferProgress Demuxer::getPreBufferProgress() const
{
    PreBufferProgress progress{};

    progress.videoRequiredFrames = preBufferVideoFrames_;
    progress.audioRequiredPackets = preBufferAudioPackets_;

    if (videoPacketQueue_) {
        progress.videoBufferedFrames = videoPacketQueue_->packetCount();
        progress.isVideoReady = progress.videoBufferedFrames >= preBufferVideoFrames_;

        const double videoProgress =
            progress.videoRequiredFrames > 0
                ? std::min(1.0, (double)progress.videoBufferedFrames / progress.videoRequiredFrames)
                : 1.0;
        progress.videoProgressPercent = videoProgress;
    }

    if (audioPacketQueue_) {
        progress.audioBufferedPackets = audioPacketQueue_->packetCount();
        progress.isAudioReady = progress.audioBufferedPackets >= preBufferAudioPackets_;

        const double audioProgress = progress.audioRequiredPackets > 0
                                         ? std::min(1.0, (double)progress.audioBufferedPackets /
                                                             progress.audioRequiredPackets)
                                         : 1.0;
        progress.audioProgressPercent = audioProgress;
    }

    if (requireBothStreams_) {
        progress.isOverallReady = progress.isVideoReady && progress.isAudioReady;
    } else {
        progress.isOverallReady = progress.isVideoReady || progress.isAudioReady;
    }

    return progress;
}

void Demuxer::start()
{
    if (isRunning_.load()) {
        return;
    }

    // 启动队列
    if (videoPacketQueue_) {
        videoPacketQueue_->start();
    }
    if (audioPacketQueue_) {
        audioPacketQueue_->start();
    }

    // 启动解复用线程
    isRunning_.store(true);
    thread_ = std::thread(&Demuxer::demuxLoop, this);

    LOG_INFO("{} demuxer started!", url_);
}

void Demuxer::stop()
{
    if (!isRunning_.load()) {
        return;
    }

    isRunning_.store(false);

    // 清理预缓冲状态
    clearPreBufferCallback();

    // 唤醒暂停的线程
    pauseCv_.notify_all();

    // 中止队列
    if (videoPacketQueue_) {
        videoPacketQueue_->abort();
    }
    if (audioPacketQueue_) {
        audioPacketQueue_->abort();
    }

    // 等待线程结束
    if (thread_.joinable()) {
        thread_.join();
    }

    LOG_INFO("{} demuxer stopped!", url_);
}

void Demuxer::demuxLoop()
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        LOG_ERROR("Failed to allocate packet");
        return;
    }

    // 根据流类型选择不同的处理策略
    if (isRealTime_.load()) {
        realTimeStreamDemuxLoop(pkt);
    } else {
        fileStreamDemuxLoop(pkt);
    }

    av_packet_free(&pkt);
    LOG_INFO("{} demux loop ended.", url_);
}

void Demuxer::fileStreamDemuxLoop(AVPacket *pkt)
{
    int errorCount = 0;
    bool readFirstPacket = false;
    std::optional<bool> isEof = std::nullopt;

    while (isRunning_.load()) {
        // 处理暂停状态 - 文件流暂停时完全停止读取
        if (!handleFileStreamPause()) {
            break;
        }

        // 读取并处理数据包
        int result = readAndProcessPacket(pkt, errorCount, readFirstPacket, isEof.value_or(false));
        if (result == 1) {
            isEof = true;
            // 文件结束
            if ((!videoPacketQueue_ || videoPacketQueue_->isEmpty()) &&
                (!audioPacketQueue_ || audioPacketQueue_->isEmpty()) && isEof.value_or(false)) {
                auto event = std::make_shared<StreamEventArgs>(url_, "Demuxer", "Stream Ended");
                eventDispatcher_->triggerEvent(EventType::kStreamEnded, event);

                // 检查是否需要循环播放（仅对文件流有效）
                if (!isRealTime_.load() && handleLoopPlayback()) {
                    // 成功开始新的循环，不发送结束包
                    av_packet_unref(pkt);
                }
                isEof.reset();
            }
            continue;
        } else if (result == -2) {
            // 严重错误，退出循环
            break;
        } else if (result == -1) {
            // 需要继续循环
            continue;
        }

        isEof.reset();
        // 分发数据包
        distributePacket(pkt);
        av_packet_unref(pkt);
    }
}

void Demuxer::realTimeStreamDemuxLoop(AVPacket *pkt)
{
    int errorCount = 0;
    bool readFirstPacket = false;

    while (isRunning_.load()) {
        // 读取并处理数据包
        int result = readAndProcessPacket(pkt, errorCount, readFirstPacket);
        if (result == 1) {
            // 实时流EOF，短暂等待后继续
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        } else if (result == -2) {
            // 严重错误，退出循环
            break;
        } else if (result == -1) {
            // 需要继续循环
            continue;
        }

        // 分发数据包
        distributePacket(pkt);
        av_packet_unref(pkt);
    }
}

bool Demuxer::handleFileStreamPause()
{
    if (isPaused_.load()) {
        std::unique_lock<std::mutex> lock(mutex_);
        pauseCv_.wait(lock, [this] { return !isPaused_.load() || !isRunning_.load(); });

        if (!isRunning_.load()) {
            return false;
        }
    }
    return true;
}

int Demuxer::readAndProcessPacket(AVPacket *pkt, int &errorCount, bool &readFirstPacket, bool isEof)
{
    const int ret = av_read_frame(formatContext_, pkt);
    
    // 成功读取数据包
    if (ret >= 0) {
        errorCount = 0;
        
        if (!readFirstPacket) {
            readFirstPacket = true;
            auto event = std::make_shared<StreamEventArgs>(url_, "Demuxer", "Stream Read Data");
            eventDispatcher_->triggerEvent(EventType::kStreamReadData, event);
        }
        
        return 0;
    }

    // 实时流暂停状态下不处理其他错误
    if (isRealTime_.load() && isPaused_.load()) {
        return -1;
    }
    
    // 处理EOF（对所有流类型都适用）
    if (ret == AVERROR_EOF || avio_feof(formatContext_->pb)) {
        if (!isEof) {
            handleEndOfFile(pkt);
        }
        
        // 实时流的EOF可能是临时的，应该累计错误计数
        if (isRealTime_.load()) {
            return handleReadError(errorCount, "EOF in real-time stream");
        }
        
        return 1; // 非实时流的正常EOF
    }
    
    // 处理EAGAIN（需要重试）
    if (ret == AVERROR(EAGAIN)) {
        return -1;
    }
    
    // 处理其他读取错误
    return handleReadError(errorCount, "Stream read error");
}

void Demuxer::handleEndOfFile(AVPacket *pkt)
{
    // 发送结束包到各个队列
    if (videoStreamIndex_ >= 0 && videoPacketQueue_) {
        pkt->stream_index = videoStreamIndex_;
        Packet endPacket(pkt);
        endPacket.setSerial(videoPacketQueue_->serial());
        videoPacketQueue_->push(endPacket, 0);
    }

    if (audioStreamIndex_ >= 0 && audioPacketQueue_) {
        pkt->stream_index = audioStreamIndex_;
        Packet endPacket(pkt);
        endPacket.setSerial(audioPacketQueue_->serial());
        audioPacketQueue_->push(endPacket, 0);
    }

    av_packet_unref(pkt);
}

void Demuxer::distributePacket(AVPacket *pkt)
{
    Packet packet(pkt);

    if (pkt->stream_index == videoStreamIndex_) {
        if (videoPacketQueue_ && !isPaused_.load()) {
            packet.setSerial(videoPacketQueue_->serial());
            videoPacketQueue_->push(packet, -1);
        }

        // 如果正在录制，写入录制器
        if (realTimeStreamRecorder_->isRecording()) {
            realTimeStreamRecorder_->writePacket(packet, AVMEDIA_TYPE_VIDEO);
        }
    } else if (pkt->stream_index == audioStreamIndex_) {
        if (audioPacketQueue_ && !isPaused_.load()) {
            packet.setSerial(audioPacketQueue_->serial());
            audioPacketQueue_->push(packet, -1);
        }

        // 如果正在录制，写入录制器
        if (realTimeStreamRecorder_->isRecording()) {
            realTimeStreamRecorder_->writePacket(packet, AVMEDIA_TYPE_AUDIO);
        }
    }
}

void Demuxer::checkPreBufferStatus()
{
    if (!preBufferEnabled_ || preBufferReady_) {
        return;
    }

    bool videoReady = true;
    bool audioReady = true;

    // 检查视频缓冲
    if (videoStreamIndex_ >= 0 && preBufferVideoFrames_ > 0) {
        videoReady = videoPacketQueue_ && videoPacketQueue_->packetCount() >= preBufferVideoFrames_;
    }

    // 检查音频缓冲
    if (audioStreamIndex_ >= 0 && preBufferAudioPackets_ > 0) {
        audioReady =
            audioPacketQueue_ && audioPacketQueue_->packetCount() >= preBufferAudioPackets_;
    }

    // 判断是否达到预缓冲要求
    bool ready = false;
    if (requireBothStreams_) {
        ready = videoReady && audioReady;
    } else {
        ready = videoReady || audioReady;
    }

    if (ready && !preBufferReady_) {
        preBufferReady_ = true;
        LOG_INFO("PreBuffer ready: video={}/{}, audio={}/{}",
                 videoPacketQueue_ ? videoPacketQueue_->packetCount() : 0, preBufferVideoFrames_,
                 audioPacketQueue_ ? audioPacketQueue_->packetCount() : 0, preBufferAudioPackets_);

        // 触发预缓冲完成回调
        if (preBufferReadyCallback_) {
            try {
                preBufferReadyCallback_();
            } catch (const std::exception &e) {
                LOG_ERROR("Exception in pre-buffer callback: {}", e.what());
            }
        }
    }
}

bool Demuxer::handleLoopPlayback()
{
    std::lock_guard<std::mutex> lock(loopMutex_);

    LoopMode mode = loopMode_.load();
    if (mode == LoopMode::kNone) {
        return false;
    }

    int currentLoop = currentLoopCount_.load();
    int maxLoops = maxLoops_.load();

    // 检查是否达到最大循环次数
    if (mode == LoopMode::kSingle && maxLoops > 0 && currentLoop >= maxLoops) {
        return false;
    }

    // 执行seek到文件开头
    seek(0.0);
    // 重置到文件开始位置
    if (formatContext_->pb && formatContext_->pb->seekable) {
        avio_seek(formatContext_->pb, 0, SEEK_SET);
    }

    // lazy, 消除竞态条件，等待同步
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 增加循环计数
    currentLoopCount_.fetch_add(1);

    // 触发循环播放事件
    auto loopEvent = std::make_shared<LoopEventArgs>(currentLoopCount_.load(), maxLoops_.load(),
                                                     "Demuxer", "Stream Looped");
    eventDispatcher_->triggerEvent(EventType::kStreamLooped, loopEvent);

    LOG_INFO("Stream looped: current={}, max={}", currentLoopCount_.load(), maxLoops_.load());
    return true;
}

int Demuxer::handleReadError(int &errorCount, const std::string& errorType)
{
    if (++errorCount >= kReadErrorMaxCount) {
        LOG_ERROR("Too many {} ({}) in {}, stopping", errorType, errorCount, url_);
        
        auto event = std::make_shared<StreamEventArgs>(url_, "Demuxer", "Stream Read Error");
        eventDispatcher_->triggerEvent(EventType::kStreamReadError, event);
        return -2; // 严重错误
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return -1; // 需要继续
}

void Demuxer::clearPreBufferCallback()
{
    std::lock_guard<std::mutex> lock(mutex_);
    preBufferReadyCallback_ = nullptr;
    preBufferEnabled_ = false;
    preBufferReady_ = false;
}

void Demuxer::setLoopMode(LoopMode mode, int maxLoops)
{
    std::lock_guard<std::mutex> lock(loopMutex_);
    loopMode_.store(mode);
    maxLoops_.store(maxLoops);
    if (mode == LoopMode::kNone) {
        currentLoopCount_.store(0);
    }
}

LoopMode Demuxer::getLoopMode() const
{
    return loopMode_.load();
}

int Demuxer::getCurrentLoopCount() const
{
    return currentLoopCount_.load();
}

void Demuxer::resetLoopCount()
{
    currentLoopCount_.store(0);
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END