#include "DecoderController.h"
#include "Logger.h"
#include "Utils.h"

DecoderController::DecoderController()
    : eventDispatcher_(std::make_shared<EventDispatcher>()),
      demuxer_(std::make_shared<Demuxer>(eventDispatcher_)),
      syncController_(std::make_shared<SyncController>()),
      reconnectAttempts_(0),
      asyncOpenInProgress_(false),
      shouldCancelAsyncOpen_(false)
{
    eventDispatcher_->startAsyncProcessing();

    // 注册事件处理函数
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
    // 取消任何正在进行的异步操作
    cancelAsyncOpen();
    close();
}

bool DecoderController::open(const std::string &filePath, const Config &config)
{
    // 取消任何正在进行的异步打开操作
    cancelAsyncOpen();

    // 停止任何正在进行的重连任务
    shouldStopReconnect_.store(true);

    // 等待重连任务结束
    while (isReconnecting_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 重置重连相关状态
    reconnectAttempts_.store(0);
    shouldStopReconnect_.store(false);
    // 重置预加载状态
    preBufferState_ = PreBufferState::Disabled;

    config_ = config;

    // 打开媒体文件，并启用解复用器
    if (!demuxer_->open(filePath, utils::isRealtime(filePath))) {
        return false;
    }

    return true;
}

void DecoderController::openAsync(const std::string &filePath,
                                  const Config &config,
                                  AsyncOpenCallback callback)
{
    // 取消任何正在进行的异步打开操作
    cancelAsyncOpen();

    // 保存回调函数
    {
        std::lock_guard<std::mutex> lock(asyncCallbackMutex_);
        asyncOpenCallback_ = callback;
    }

    // 标记异步操作开始
    asyncOpenInProgress_.store(true);
    shouldCancelAsyncOpen_.store(false);

    // 创建异步任务
    asyncOpenFuture_ =
        std::async(std::launch::async, [this, filePath, config]() {
            AsyncOpenResult result = AsyncOpenResult::Failed;
            bool openSuccess = false;
            std::string errorMessage;

            try {
                // 检查是否需要取消
                if (shouldCancelAsyncOpen_.load()) {
                    result = AsyncOpenResult::Cancelled;
                    errorMessage = "Operation was cancelled before starting";
                } else {
                    // 执行实际的打开操作
                    openSuccess = openAsyncInternal(filePath, config);

                    if (shouldCancelAsyncOpen_.load()) {
                        result = AsyncOpenResult::Cancelled;
                        errorMessage =
                            "Operation was cancelled during execution";
                        // 如果打开成功但被取消，需要关闭
                        if (openSuccess) {
                            demuxer_->close();
                            openSuccess = false;
                        }
                    } else if (openSuccess) {
                        result = AsyncOpenResult::Success;
                    } else {
                        result = AsyncOpenResult::Failed;
                        errorMessage = "Failed to open media file";
                    }
                }
            } catch (const std::exception &e) {
                result = AsyncOpenResult::Failed;
                openSuccess = false;
                errorMessage = std::string("Exception occurred: ") + e.what();
            }

            // 调用回调函数
            AsyncOpenCallback callback;
            {
                std::lock_guard<std::mutex> lock(asyncCallbackMutex_);
                callback = asyncOpenCallback_;
                asyncOpenCallback_ = nullptr; // 清空回调
            }

            if (callback) {
                callback(result, openSuccess, errorMessage);
            }

            // 重置状态
            asyncOpenInProgress_.store(false);
        });
}

void DecoderController::cancelAsyncOpen()
{
    if (asyncOpenInProgress_.load()) {
        // 设置取消标志
        shouldCancelAsyncOpen_.store(true);

        // 等待异步操作完成（这会触发回调）
        if (asyncOpenFuture_.valid()) {
            try {
                asyncOpenFuture_.wait();
            } catch (const std::exception &e) {
                LOG_ERROR(
                    "Exception while waiting for async open to complete: {}",
                    e.what());
            }
        }

        // 重置状态
        shouldCancelAsyncOpen_.store(false);
    }
}

bool DecoderController::isAsyncOpenInProgress() const
{
    return asyncOpenInProgress_.load();
}

bool DecoderController::openAsyncInternal(const std::string &filePath,
                                          const Config &config)
{
    // 检查是否需要取消
    if (shouldCancelAsyncOpen_.load()) {
        return false;
    }

    // 停止任何正在进行的重连任务
    shouldStopReconnect_.store(true);

    // 等待重连任务结束
    while (isReconnecting_.load()) {
        if (shouldCancelAsyncOpen_.load()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 再次检查是否需要取消
    if (shouldCancelAsyncOpen_.load()) {
        return false;
    }

    // 重置重连相关状态
    reconnectAttempts_.store(0);
    shouldStopReconnect_.store(false);
    // 重置预加载状态
    preBufferState_ = PreBufferState::Disabled;

    config_ = config;

    // 检查是否需要取消
    if (shouldCancelAsyncOpen_.load()) {
        return false;
    }

    // 打开媒体文件，并启用解复用器
    return demuxer_->open(filePath, utils::isRealtime(filePath));
}

bool DecoderController::close()
{
    // 取消任何正在进行的异步打开操作
    cancelAsyncOpen();

    // 停止任何正在进行的重连任务
    shouldStopReconnect_.store(true);

    // 等待重连任务结束
    while (isReconnecting_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 清理预缓冲状态
    cleanupPreBufferState();

    // 停止解码
    stopDecode();

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
    if (!demuxer_) {
        return false;
    }

    // 启动解码器（非阻塞）
    bool success = startDecodeInternal(false);
    if (!success) {
        return false;
    }

    // 如果启用了预缓冲，设置等待状态
    if (config_.preBufferConfig.enablePreBuffer) {
        preBufferState_ = PreBufferState::WaitingBuffer;

        // 设置解码器等待预缓冲
        if (videoDecoder_) {
            videoDecoder_->setWaitingForPreBuffer(true);
        }
        if (audioDecoder_) {
            audioDecoder_->setWaitingForPreBuffer(true);
        }

        // 设置预缓冲配置和完成回调
        demuxer_->setPreBufferConfig(
            config_.preBufferConfig.videoPreBufferFrames,
            config_.preBufferConfig.audioPreBufferPackets,
            config_.preBufferConfig.requireBothStreams, [this]() {
                // 预缓冲完成回调
                this->onPreBufferReady();
            });

        LOG_INFO("Decoder started, waiting for pre-buffer to complete...");
    }

    return true;
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
    eventDispatcher_->triggerEvent(EventType::kSeekStarted, event);

    const auto sendFailedEvent = [this, position]() {
        auto event = std::make_shared<SeekEventArgs>(
            syncController_->getMasterClock(), position, "DecoderController",
            "Seek Failed");
        eventDispatcher_->triggerEvent(EventType::kSeekFailed, event);
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
    eventDispatcher_->triggerEvent(EventType::kSeekSuccess, event);

    return result;
}

bool DecoderController::setSpeed(double speed)
{
    if (speed <= 0.0f) {
        return false;
    }

    // 如果是实时流，则不支持设置速度
    if (!demuxer_ || demuxer_->isRealTime()) {
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

GlobalEventListenerHandle DecoderController::addGlobalEventListener(
    EventCallback callback)
{
    return eventDispatcher_->addGlobalEventListener(callback);
}

bool DecoderController::removeGlobalEventListener(
    const GlobalEventListenerHandle &handle)
{
    return eventDispatcher_->removeGlobalEventListener(handle);
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

void DecoderController::processAsyncEvents()
{
    eventDispatcher_->processAsyncEvents();
}

void DecoderController::startAsyncProcessing()
{
    eventDispatcher_->startAsyncProcessing();
}

void DecoderController::stopAsyncProcessing()
{
    eventDispatcher_->stopAsyncProcessing();
}

bool DecoderController::isAsyncProcessingActive() const
{
    return eventDispatcher_->isAsyncProcessingActive();
}

std::vector<EventType> DecoderController::allEventTypes() const
{
    return EventDispatcher::allEventTypes();
}

std::string DecoderController::getEventTypeName(EventType type) const
{
    return EventDispatcher::getEventTypeName(type);
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

DecoderController::PreBufferState DecoderController::getPreBufferState() const
{
    return preBufferState_;
}

DecoderController::PreBufferProgress DecoderController::getPreBufferProgress()
    const
{
    PreBufferProgress progress{};

    if (demuxer_) {
        auto demuxProgress = demuxer_->getPreBufferProgress();
        progress.videoBufferedFrames = demuxProgress.videoBufferedFrames;
        progress.audioBufferedPackets = demuxProgress.audioBufferedPackets;
        progress.videoRequiredFrames = demuxProgress.videoRequiredFrames;
        progress.audioRequiredPackets = demuxProgress.audioRequiredPackets;
        progress.isVideoReady = demuxProgress.isVideoReady;
        progress.isAudioReady = demuxProgress.isAudioReady;
        progress.isOverallReady = demuxProgress.isOverallReady;

        // 计算总体进度
        double videoProgress =
            progress.videoRequiredFrames > 0
                ? std::min(1.0, (double)progress.videoBufferedFrames /
                                    progress.videoRequiredFrames)
                : 1.0;
        double audioProgress =
            progress.audioRequiredPackets > 0
                ? std::min(1.0, (double)progress.audioBufferedPackets /
                                    progress.audioRequiredPackets)
                : 1.0;

        if (config_.preBufferConfig.requireBothStreams) {
            progress.progressPercent = std::min(videoProgress, audioProgress);
        } else {
            progress.progressPercent = std::max(videoProgress, audioProgress);
        }
    }

    return progress;
}

bool DecoderController::reopen(const std::string &url)
{
    LOG_INFO("Start reconnect stream: {}", url);
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
        LOG_INFO("reconnect success: {}", url);
    } else {
        LOG_ERROR("reconnect failed: {}", url);
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
            LOG_INFO("Receive stop reconnect signal, stop reconnect task: {}",
                     url);
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
        LOG_ERROR("Reached max reconnect times {}, stop reconnect!",
                  config_.maxReconnectAttempts);
        reconnectAttempts_.store(0);
        isReconnecting_.store(false);
        return;
    }

    reconnectAttempts_++;
    LOG_INFO("Try to {} reconnect: {}", reconnectAttempts_.load(), url);

    if (reopen(url)) {
        reconnectAttempts_.store(0); // 重连成功，重置计数器
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
    // 清理预缓冲状态（如果不是重新打开）
    if (!reopen) {
        cleanupPreBufferState();
    }

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

void DecoderController::onPreBufferReady()
{
    preBufferState_ = PreBufferState::Ready;

    // 恢复解码器
    if (videoDecoder_) {
        videoDecoder_->setWaitingForPreBuffer(false);
    }
    if (audioDecoder_) {
        audioDecoder_->setWaitingForPreBuffer(false);
    }

    LOG_INFO("Pre-buffer completed, decoders resumed");
}

void DecoderController::cleanupPreBufferState()
{
    // 重置预缓冲状态
    preBufferState_ = PreBufferState::Disabled;

    // 清理解码器的预缓冲等待状态
    if (videoDecoder_) {
        videoDecoder_->setWaitingForPreBuffer(false);
    }
    if (audioDecoder_) {
        audioDecoder_->setWaitingForPreBuffer(false);
    }

    // 清理Demuxer的预缓冲回调
    if (demuxer_) {
        demuxer_->clearPreBufferCallback();
    }

    LOG_INFO("Pre-buffer state cleaned up");
}