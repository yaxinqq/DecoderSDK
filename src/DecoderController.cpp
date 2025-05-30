#include "DecoderController.h"
#include "Logger.h"
#include "Utils.h"

DecoderController::DecoderController()
    : eventDispatcher_(std::make_shared<EventDispatcher>()),
      demuxer_(std::make_shared<Demuxer>(eventDispatcher_)),
      syncController_(std::make_shared<SyncController>()),
      reconnectAttempts_(0)  // 添加重连计数器
{
    eventDispatcher_->addEventListener(
        EventType::kStreamReadError,
        [this](EventType, std::shared_ptr<EventArgs> event) {
            StreamEventArgs *streamEvent =
                dynamic_cast<StreamEventArgs *>(event.get());
            if (streamEvent && config_.enableAutoReconnect &&
                !shouldStopReconnect_.load()) {
                // 设置重连状态
                isReconnecting_.store(true);
                // 异步执行重连，避免死锁
                std::thread([this, url = streamEvent->filePath]() {
                    handleReconnect(url);
                }).detach();
            }
        });
}

DecoderController::~DecoderController()
{
    stopDecode();
    close();
}

bool DecoderController::open(const std::string &filePath, const Config &config)
{
    // 停止任何正在进行的重连任务
    shouldStopReconnect_.store(true);

    // 等待重连任务结束
    while (isReconnecting_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 重置重连相关状态
    reconnectAttempts_.store(0);
    shouldStopReconnect_.store(false);

    config_ = config;

    // 打开媒体文件，并启用解复用器
    if (!demuxer_->open(filePath, utils::isRealtime(filePath))) {
        return false;
    }

    return true;
}

bool DecoderController::close()
{
    // 停止任何正在进行的重连任务
    shouldStopReconnect_.store(true);

    // 等待重连任务结束
    while (isReconnecting_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 重置重连相关状态
    reconnectAttempts_.store(0);
    shouldStopReconnect_.store(false);

    const auto ret = demuxer_->close();

    return ret;
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
    return startDecodeInternal(false);
}

bool DecoderController::stopDecode()
{
    return stopDecodeInternal(false);
}

bool DecoderController::seek(double position)
{
    // 发送开始seek的事件
    auto event = std::make_shared<SeekEventArgs>(
        syncController_->getMasterClock(), position, "DecoderController",
        "Seek Started");
    eventDispatcher_->triggerEventAsync(EventType::kSeekStarted, event);

    const auto sendFailedEvent = [this, position]() {
        auto event = std::make_shared<SeekEventArgs>(
            syncController_->getMasterClock(), position, "DecoderController",
            "Seek Failed");
        eventDispatcher_->triggerEventAsync(EventType::kSeekFailed, event);
    };

    if (!demuxer_) {
        sendFailedEvent();
        return false;
    }

    // 如果是实时流，则不支持seek
    if (demuxer_->isRealTime()) {
        sendFailedEvent();
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

    // 发送seek成功的事件
    event = std::make_shared<SeekEventArgs>(syncController_->getMasterClock(),
                                            position, "DecoderController",
                                            "Seek Success");
    eventDispatcher_->triggerEventAsync(EventType::kSeekSuccess, event);

    return result;
}

bool DecoderController::setSpeed(double speed)
{
    if (speed <= 0.0f) {
        return false;
    }

    // 如果是实时流，则不支持设置速度
    if (!demuxer_ || !demuxer_->isRealTime()) {
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

FrameQueue &DecoderController::videoQueue()
{
    static FrameQueue emptyQueue;
    return videoDecoder_ ? videoDecoder_->frameQueue() : emptyQueue;
}

FrameQueue &DecoderController::audioQueue()
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

bool DecoderController::startRecording(const std::string &outputPath)
{
    if (!demuxer_)
        return false;

    return demuxer_->startRecording(outputPath);
}

bool DecoderController::stopRecording()
{
    if (!demuxer_)
        return false;

    return demuxer_->stopRecording();
}

bool DecoderController::isRecording() const
{
    return demuxer_ && demuxer_->isRecording();
}

EventListenerHandle DecoderController::addGlobalEventListener(
    EventCallback callback)
{
    return eventDispatcher_->addGlobalEventListener(callback);
}

bool DecoderController::removeGlobalEventListener(EventListenerHandle handle)
{
    return eventDispatcher_->removeGlobalEventListener(handle);
}

size_t DecoderController::getGlobalListenerCount() const
{
    return eventDispatcher_->getGlobalListenerCount();
}

EventListenerHandle DecoderController::addEventListener(EventType eventType,
                                                        EventCallback callback)
{
    return eventDispatcher_->addEventListener(eventType, callback);
}

bool DecoderController::removeEventListener(EventType eventType,
                                            EventListenerHandle handle)
{
    return eventDispatcher_->removeEventListener(eventType, handle);
}

void DecoderController::removeAllListeners()
{
    eventDispatcher_->removeAllListeners();
}

void DecoderController::setAsyncProcessing(bool enabled)
{
    eventDispatcher_->setAsyncProcessing(enabled);
}

void DecoderController::stopReconnect()
{
    shouldStopReconnect_.store(true);

    // 等待重连任务结束
    while (isReconnecting_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 重置状态
    reconnectAttempts_.store(0);
    shouldStopReconnect_.store(false);
}

bool DecoderController::isReconnecting() const
{
    return isReconnecting_.load();
}

bool DecoderController::reopen(const std::string &url)
{
    LOG_INFO("开始重连流: {}", url);
    isReconnecting_.store(true);

    // 停止当前解码
    bool wasDecoding = isStartDecoding_;
    if (wasDecoding) {
        stopDecodeInternal(true);
    }

    // 关闭当前连接
    demuxer_->close();

    // 等待一段时间后重新连接
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // 重新打开流，标记为重连
    bool reopenSuccess = demuxer_->open(url, utils::isRealtime(url), true);

    if (reopenSuccess && wasDecoding) {
        // 如果重连成功且之前在解码，则重新开始解码
        startDecodeInternal(true);
    }

    if (reopenSuccess) {
        LOG_INFO("重连成功: {}", url);
    } else {
        LOG_ERROR("重连失败: {}", url);
    }

    isReconnecting_.store(!reopenSuccess);
    return reopenSuccess;
}

// 添加新的重连处理方法
void DecoderController::handleReconnect(const std::string &url)
{
    const auto checkStopReconnection = [this, url]() {
        // 检查是否需要停止重连
        if (shouldStopReconnect_.load()) {
            LOG_INFO("收到停止重连信号，终止重连任务: {}", url);
            isReconnecting_.store(false);
            reconnectAttempts_.store(0);
            return true;
        }
        return false;
    };

    // 检查是否需要停止重连
    if (checkStopReconnection()) {
        return;
    }

    if (config_.maxReconnectAttempts >= 0 &&
        reconnectAttempts_ >= config_.maxReconnectAttempts) {
        LOG_ERROR("达到最大重连次数 {}, 停止重连",
                  config_.maxReconnectAttempts);
        reconnectAttempts_.store(0);
        isReconnecting_.store(false);
        return;
    }

    reconnectAttempts_++;
    LOG_INFO("第 {} 次重连尝试: {}", reconnectAttempts_.load(), url);

    if (reopen(url)) {
        reconnectAttempts_.store(0);  // 重连成功，重置计数器
        isReconnecting_.store(false);
    } else {
        // 重连失败，等待后再次尝试
        // 在等待期间也要检查停止标志
        for (int i = 0; i < config_.reconnectIntervalMs; i += 100) {
            if (checkStopReconnection()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (checkStopReconnection()) {
            return;
        }
        handleReconnect(url);
    }
}

bool DecoderController::startDecodeInternal(bool reopen)
{
    // 如果当前已开始解码，则先停止
    if (isStartDecoding_) {
        stopDecodeInternal(reopen);
    }

    // 重置同步控制器
    syncController_->resetClocks();

    // 创建视频解码器
    if (demuxer_->hasVideo()) {
        videoDecoder_ = std::make_shared<VideoDecoder>(
            demuxer_, syncController_, eventDispatcher_);
        videoDecoder_->init(config_.hwAccelType, config_.hwDeviceIndex,
                            config_.videoOutFormat,
                            config_.requireFrameInSystemMemory);
        videoDecoder_->setFrameRateControl(config_.enableFrameRateControl);
        videoDecoder_->setSpeed(config_.speed);
        if (!videoDecoder_->open()) {
            return false;
        }
    }

    // 创建音频解码器
    if (demuxer_->hasAudio()) {
        audioDecoder_ = std::make_shared<AudioDecoder>(
            demuxer_, syncController_, eventDispatcher_);
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

    if (!reopen) {
        isStartDecoding_ = true;
    }

    return true;
}

bool DecoderController::stopDecodeInternal(bool reopen)
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

    if (!reopen) {
        isStartDecoding_ = false;
    }

    return true;
}