#include "decoder_controller.h"

extern "C" {
#include "libavdevice/avdevice.h"
}

#include "logger/logger.h"
#include "utils/common_utils.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

DecoderController::DecoderController()
    : eventDispatcher_(std::make_shared<EventDispatcher>()),
      syncController_(std::make_shared<StreamSyncManager>()),
      demuxer_(std::make_shared<Demuxer>(eventDispatcher_)),
      asyncOpenInProgress_(false),
      shouldCancelAsyncOpen_(false)
{
    // 注册ffmpeg
    avdevice_register_all();
    avformat_network_init();

    // 开启异步线程处理函数
    eventDispatcher_->startAsyncProcessing();

    // 注册事件处理函数 - 新增重连逻辑
    eventDispatcher_->addEventListener(
        EventType::kStreamReadError, [this](EventType, std::shared_ptr<EventArgs> event) {
            StreamEventArgs *streamEvent = dynamic_cast<StreamEventArgs *>(event.get());
            if (streamEvent && config_.enableAutoReconnect && !shouldStopReconnect_.load()) {
                LOG_WARN("Stream read error detected, starting reconnect for: {}",
                         streamEvent->filePath);
                startReconnect();
            }
        });
}

DecoderController::~DecoderController()
{
    {
        std::lock_guard<std::mutex> lock(asyncCallbackMutex_);
        asyncOpenCallback_ = nullptr;
    }

    // 停止重连
    stopReconnect();
    // 等待重连线程结束，有可能在reconnectLoop中就重连好了，但没有join
    if (reconnectThread_.joinable())
        reconnectThread_.join();

    // 取消任何正在进行的异步操作
    cancelAsyncOpen();
    close();

    avformat_network_deinit();
}

bool DecoderController::open(const std::string &url, const Config &config)
{
    // 取消任何正在进行的异步打开操作
    cancelAsyncOpen();

    // 停止任何正在进行的重连
    stopReconnect();

    std::lock_guard<std::mutex> lock(mutex_);
    return openInternal(url, config);
}

void DecoderController::openAsync(const std::string &url, const Config &config,
                                  AsyncOpenCallback callback)
{
    // 取消任何正在进行的异步打开操作
    cancelAsyncOpen();

    std::lock_guard<std::mutex> lock(mutex_);

    // 保存回调函数
    {
        std::lock_guard<std::mutex> lock(asyncCallbackMutex_);
        asyncOpenCallback_ = callback;
    }

    // 标记异步操作开始
    asyncOpenInProgress_.store(true);
    shouldCancelAsyncOpen_.store(false);

    // 创建异步任务
    asyncOpenFuture_ = std::async(std::launch::async, [this, url, config]() {
        AsyncOpenResult result = AsyncOpenResult::kFailed;
        bool openSuccess = false;
        std::string errorMessage;

        try {
            std::lock_guard<std::mutex> lock(mutex_);

            // 检查是否需要取消
            if (shouldCancelAsyncOpen_.load()) {
                result = AsyncOpenResult::kCancelled;
                errorMessage = "Operation was canceled before starting";
            } else {
                // 执行实际的打开操作
                openSuccess = openAsyncInternal(url, config);

                if (shouldCancelAsyncOpen_.load()) {
                    result = AsyncOpenResult::kCancelled;
                    errorMessage = "Operation was canceled during execution";
                    // 如果打开成功但被取消，需要关闭
                    if (openSuccess) {
                        demuxer_->close();
                        openSuccess = false;
                    }
                } else if (openSuccess) {
                    result = AsyncOpenResult::kSuccess;
                } else {
                    result = AsyncOpenResult::kFailed;
                    errorMessage = "Failed to open media file";
                }
            }
        } catch (const std::exception &e) {
            result = AsyncOpenResult::kFailed;
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
                LOG_ERROR("Exception while waiting for async open to complete: {}", e.what());
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

bool DecoderController::close()
{
    // 停止重连
    stopReconnect();

    // 取消任何正在进行的异步打开操作
    cancelAsyncOpen();

    // 清理预缓冲状态
    cleanupPreBufferState();

    // 停止解码
    stopDecode();

    std::lock_guard<std::mutex> lock(mutex_);
    return closeInternal();
}

bool DecoderController::pause()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!demuxer_) {
        return false;
    }

    if (videoDecoder_)
        videoDecoder_->pause();
    if (audioDecoder_)
        audioDecoder_->pause();

    isDemuxerPausedWhenReconnected_.store(true);
    return demuxer_->pause();
}

bool DecoderController::resume()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!demuxer_) {
        return false;
    }

    if (demuxer_->isRealTime()) {
        syncController_->resetClocks();
    }

    if (videoDecoder_)
        videoDecoder_->resume();
    if (audioDecoder_)
        audioDecoder_->resume();

    isDemuxerPausedWhenReconnected_.store(false);
    return demuxer_->resume();
}

bool DecoderController::startDecode()
{
    std::lock_guard<std::mutex> lock(mutex_);
    hasDecoderWhenReconnected_.store(true);
    return startDecodeInternal();
}

bool DecoderController::stopDecode()
{
    std::lock_guard<std::mutex> lock(mutex_);
    hasDecoderWhenReconnected_.store(false);
    return stopDecodeInternal();
}

bool DecoderController::isDecodeStopped() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return isDecoding_;
}

bool DecoderController::isPaused() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return demuxer_->isPaused();
}

bool DecoderController::seek(double position)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 发送开始seek的事件
    auto event = std::make_shared<SeekEventArgs>(syncController_->getMasterClock(), position,
                                                 "DecoderController", "Seek Started");
    eventDispatcher_->triggerEvent(EventType::kSeekStarted, event);

    const auto sendFailedEvent = [this, position]() {
        auto event = std::make_shared<SeekEventArgs>(syncController_->getMasterClock(), position,
                                                     "DecoderController", "Seek Failed");
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
    /*bool wasPaused = false;
    if (videoDecoder_ || audioDecoder_) {
        wasPaused = demuxer_->isPaused();
        if (!wasPaused) {
            pause();
        }
    }*/

    // 执行seek操作
    bool result = demuxer_->seek(position);

    if (result) {
        // 先重置同步控制器的时钟
        syncController_->resetClocks();

        // 清空队列，并设置seek节点
        if (videoDecoder_) {
            videoDecoder_->frameQueue()->clear();
            videoDecoder_->setSeekPos(position);
        }
        if (audioDecoder_) {
            audioDecoder_->frameQueue()->clear();
            audioDecoder_->setSeekPos(position);
        }

        // 重新初始化时钟基准
        if (audioDecoder_) {
            syncController_->updateAudioClock(position,
                                              demuxer_->packetQueue(AVMEDIA_TYPE_AUDIO)->serial());
        }
        if (videoDecoder_) {
            syncController_->updateVideoClock(position,
                                              demuxer_->packetQueue(AVMEDIA_TYPE_VIDEO)->serial());
        }
    }

    // 如果之前没有暂停，则恢复播放
    /*if (!wasPaused) {
        resume();
    }*/

    // 发送seek成功的事件
    event = std::make_shared<SeekEventArgs>(syncController_->getMasterClock(), position,
                                            "DecoderController", "Seek Success");
    eventDispatcher_->triggerEvent(EventType::kSeekSuccess, event);

    return result;
}

bool DecoderController::setSpeed(double speed)
{
    std::lock_guard<std::mutex> lock(mutex_);

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

std::shared_ptr<FrameQueue> DecoderController::videoQueue()
{
    static std::shared_ptr<FrameQueue> emptyQueue = std::make_shared<FrameQueue>(0, false);
    return videoDecoder_ ? videoDecoder_->frameQueue() : emptyQueue;
}

std::shared_ptr<FrameQueue> DecoderController::audioQueue()
{
    static std::shared_ptr<FrameQueue> emptyQueue = std::make_shared<FrameQueue>(0, false);
    return audioDecoder_ ? audioDecoder_->frameQueue() : emptyQueue;
}

void DecoderController::setMasterClock(MasterClock type)
{
    syncController_->setMaster(type);
}

double DecoderController::getVideoFrameRate() const
{
    return videoDecoder_ ? videoDecoder_->getFrameRate() : 0.0;
}

void DecoderController::setFrameRateControl(bool enable)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_.enableFrameRateControl = enable;
    }

    if (videoDecoder_) {
        videoDecoder_->setFrameRateControl(enable);
    }
}

bool DecoderController::isFrameRateControlEnabled() const
{
    return videoDecoder_ ? videoDecoder_->isFrameRateControlEnabled() : false;
}

double DecoderController::curSpeed() const
{
    std::lock_guard<std::mutex> lock(mutex_);
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
    const std::function<EventCallback> &callback)
{
    return eventDispatcher_->addGlobalEventListener(callback);
}

bool DecoderController::removeGlobalEventListener(const GlobalEventListenerHandle &handle)
{
    return eventDispatcher_->removeGlobalEventListener(handle);
}

EventListenerHandle DecoderController::addEventListener(
    EventType eventType, const std::function<EventCallback> &callback)
{
    return eventDispatcher_->addEventListener(eventType, callback);
}

bool DecoderController::removeEventListener(EventType eventType, EventListenerHandle handle)
{
    return eventDispatcher_->removeEventListener(eventType, handle);
}

bool DecoderController::processAsyncEvents()
{
    return eventDispatcher_->processAsyncEvents();
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

bool DecoderController::isRealTimeUrl() const
{
    return demuxer_ ? demuxer_->isRealTime() : false;
}

bool DecoderController::setLoopMode(LoopMode mode, int maxLoops)
{
    if (!demuxer_) {
        return false;
    }

    // 只有文件流才支持循环播放
    if (isRealTimeUrl()) {
        LOG_WARN("Loop mode is not supported for real-time streams");
        return false;
    }

    demuxer_->setLoopMode(mode, maxLoops);
    return true;
}

LoopMode DecoderController::getLoopMode() const
{
    if (!demuxer_) {
        return LoopMode::kNone;
    }
    return demuxer_->getLoopMode();
}

int DecoderController::getCurrentLoopCount() const
{
    if (!demuxer_) {
        return 0;
    }
    return demuxer_->getCurrentLoopCount();
}

bool DecoderController::resetLoopCount()
{
    if (!demuxer_) {
        return false;
    }
    demuxer_->resetLoopCount();
    return true;
}

bool DecoderController::isReconnecting() const
{
    return isReconnecting_.load();
}

void DecoderController::stopReconnectManually()
{
    stopReconnect();
}

PreBufferState DecoderController::getPreBufferState() const
{
    return preBufferState_;
}

PreBufferProgress DecoderController::getPreBufferProgress() const
{
    return demuxer_ ? demuxer_->getPreBufferProgress() : PreBufferProgress();
}

bool DecoderController::openInternal(const std::string &url, const Config &config)
{
    // 保存原始URL用于重连
    originalUrl_ = url;

    // 重置预加载状态
    preBufferState_ = PreBufferState::kDisabled;

    config_ = config;

    // 打开媒体文件，并启用解复用器
    if (!demuxer_->open(url, config, std::bind(&DecoderController::onPreBufferReady, this))) {
        return false;
    }

    // 根据配置初始化解码器
    if (demuxer_->hasVideo() && (config_.decodeMediaType & Config::DecodeMediaType::kVideo)) {
        videoDecoder_ = std::make_shared<VideoDecoder>(demuxer_, syncController_, eventDispatcher_);
    }

    // 开启音频解码器
    if (demuxer_->hasAudio() && (config_.decodeMediaType & Config::DecodeMediaType::kAudio)) {
        audioDecoder_ = std::make_shared<AudioDecoder>(demuxer_, syncController_, eventDispatcher_);
    }
    return true;
}

bool DecoderController::openAsyncInternal(const std::string &url, const Config &config)
{
    // 检查是否需要取消
    if (shouldCancelAsyncOpen_.load()) {
        return false;
    }

    return openInternal(url, config);
}

bool DecoderController::closeInternal()
{
    // 析构解码器
    if (videoDecoder_) {
        videoDecoder_.reset();
    }
    if (audioDecoder_) {
        audioDecoder_.reset();
    }

    return demuxer_->close();
}

bool DecoderController::startDecodeInternal()
{
    // 重置同步控制器
    syncController_->resetClocks();

    // 开启视频解码器
    if (demuxer_->hasVideo() && (config_.decodeMediaType & Config::DecodeMediaType::kVideo) &&
        videoDecoder_) {
        videoDecoder_->init(config_);
        videoDecoder_->setFrameRateControl(config_.enableFrameRateControl);
        videoDecoder_->setSpeed(config_.speed);
        if (!videoDecoder_->open()) {
            return false;
        }
    }

    // 开启音频解码器
    if (demuxer_->hasAudio() && (config_.decodeMediaType & Config::DecodeMediaType::kAudio) &&
        audioDecoder_) {
        audioDecoder_->init(config_);
        audioDecoder_->setSpeed(config_.speed);
        if (!audioDecoder_->open()) {
            return false;
        }
    }

    // 默认使用音频作为主时钟
    if (demuxer_->hasAudio() && audioDecoder_) {
        syncController_->setMaster(MasterClock::kAudio);
    } else if (demuxer_->hasVideo() && videoDecoder_) {
        syncController_->setMaster(MasterClock::kVideo);
    }

    // 如果启用了预缓冲，设置等待状态
    if (config_.preBufferConfig.enablePreBuffer) {
        preBufferState_ = PreBufferState::kWaitingBuffer;

        // 设置解码器等待预缓冲
        if (videoDecoder_) {
            videoDecoder_->setWaitingForPreBuffer(true);
        }
        if (audioDecoder_) {
            audioDecoder_->setWaitingForPreBuffer(true);
        }

        LOG_INFO("Decoder started, waiting for pre-buffer to complete...");
    }

    // 启动解码器
    if (videoDecoder_) {
        videoDecoder_->start();
    }

    if (audioDecoder_) {
        audioDecoder_->start();
    }

    isDecoding_ = true;

    return true;
}

bool DecoderController::stopDecodeInternal()
{
    // 停止解码器
    if (videoDecoder_) {
        videoDecoder_->stop();
        videoDecoder_->close();
    }

    if (audioDecoder_) {
        audioDecoder_->stop();
        audioDecoder_->close();
    }

    isDecoding_ = false;

    return true;
}

void DecoderController::onPreBufferReady()
{
    preBufferState_ = PreBufferState::kReady;

    // 检查是否需要自动开启解码
    if (config_.preBufferConfig.autoStartAfterPreBuffer) {
        // 恢复解码器
        if (videoDecoder_) {
            videoDecoder_->setWaitingForPreBuffer(false);
        }
        if (audioDecoder_) {
            audioDecoder_->setWaitingForPreBuffer(false);
        }

        LOG_INFO("Pre-buffer completed, decoders auto-started");
    } else {
        // 如果不自动开启，保持等待状态，需要手动调用恢复
        LOG_INFO("Pre-buffer completed, waiting for manual start");
    }
}

void DecoderController::cleanupPreBufferState()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 重置预缓冲状态
    preBufferState_ = PreBufferState::kDisabled;

    // 清理解码器的预缓冲等待状态
    if (videoDecoder_) {
        videoDecoder_->setWaitingForPreBuffer(false);
    }
    if (audioDecoder_) {
        audioDecoder_->setWaitingForPreBuffer(false);
    }
}

void DecoderController::startReconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 如果已经在重连中，则不重复启动
    if (isReconnecting_.load()) {
        return;
    }

    // 如果重连被禁用，则不启动重连
    if (!config_.enableAutoReconnect) {
        return;
    }

    // 设置重连状态
    isReconnecting_.store(true);
    shouldStopReconnect_.store(false);
    currentReconnectAttempt_.store(0);

    // 启动重连线程
    if (reconnectThread_.joinable()) {
        reconnectThread_.join();
    }
    reconnectThread_ = std::thread(&DecoderController::reconnectLoop, this);

    LOG_INFO("Reconnect started for URL: {}", originalUrl_);
}

void DecoderController::stopReconnect()
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (!isReconnecting_.load()) {
        return;
    }

    // 设置停止标志
    shouldStopReconnect_.store(true);

    // 等待重连线程结束
    if (reconnectThread_.joinable()) {
        lock.unlock();
        reconnectThread_.join();
        lock.lock();
    }

    cleanupReconnectState();
    isReconnecting_.store(false);
    LOG_INFO("Reconnect stopped for URL: {}", originalUrl_);
}

void DecoderController::reconnectLoop()
{
    while (!shouldStopReconnect_.load()) {
        // 检查是否达到最大重连次数
        int currentAttempt = currentReconnectAttempt_.load();
        if (config_.maxReconnectAttempts > 0 && currentAttempt >= config_.maxReconnectAttempts) {
            LOG_INFO("Max reconnect attempts ({}) reached for URL: {}",
                     config_.maxReconnectAttempts, originalUrl_);
            break;
        }

        // 增加重连次数
        currentReconnectAttempt_.fetch_add(1);
        currentAttempt = currentReconnectAttempt_.load();

        // 触发重连开始事件
        LOG_INFO("Attempting reconnect {}/{} for URL: {}", currentAttempt,
                 config_.maxReconnectAttempts > 0 ? std::to_string(config_.maxReconnectAttempts)
                                                  : "unlimited",
                 originalUrl_);

        // 尝试重连
        if (attemptReconnect()) {
            // 重连成功
            LOG_INFO("Reconnect successful after {} attempts for URL: {}", currentAttempt,
                     originalUrl_);
            break;
        } else {
            LOG_WARN("Reconnect attempt {} failed for URL: {}", currentAttempt, originalUrl_);
        }

        // 等待重连间隔
        auto waitStart = std::chrono::steady_clock::now();
        while (!shouldStopReconnect_.load()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - waitStart)
                               .count();
            if (elapsed >= config_.reconnectIntervalMs) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // 如果是被手动停止的，触发中止事件
    if (shouldStopReconnect_.load()) {
        LOG_INFO("Reconnect aborted for URL: {}", originalUrl_);
    }

    cleanupReconnectState();
    isReconnecting_.store(false);
}

bool DecoderController::attemptReconnect()
{
    try {
        // 先关闭当前连接
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // 停止解码
            stopDecodeInternal();

            // 关闭解复用器
            if (!demuxer_->close()) {
                return false;
            }
        }

        // 短暂等待，确保资源完全释放
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 检查是否应该停止重连
        if (shouldStopReconnect_.load()) {
            return false;
        }

        // 尝试重新打开
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!demuxer_->open(originalUrl_, config_,
                                std::bind(&DecoderController::onPreBufferReady, this))) {
                return false;
            }

            // 如果之前的demuxer是暂停状态，则重新暂停
            if (isDemuxerPausedWhenReconnected_.load()) {
                demuxer_->pause();
            }

            // 如果重连之前，存在解码器，则开启
            if (hasDecoderWhenReconnected_.load()) {
                if (!startDecodeInternal()) {
                    return false;
                }
            }
        }

        return true;
    } catch (const std::exception &e) {
        LOG_ERROR("Exception during reconnect attempt: {}", e.what());
        return false;
    }
}

void DecoderController::cleanupReconnectState()
{
    isReconnecting_.store(false);
    shouldStopReconnect_.store(false);
    currentReconnectAttempt_.store(0);
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END