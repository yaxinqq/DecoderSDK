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

    // 查找当前可用的硬解码器类型
    HardwareAccel::getSupportedHWAccelTypes();

    // 开启异步线程处理函数
    eventDispatcher_->startAsyncProcessing();
    LOG_DEBUG("Event dispatcher async processing started");

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

    LOG_INFO("DecoderController initialized successfully");
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
    if (reconnectThread_.joinable()) {
        LOG_DEBUG("Waiting for reconnect thread to join");
        reconnectThread_.join();
    }

    // 取消任何正在进行的异步操作
    cancelAsyncOpen();
    close();

    avformat_network_deinit();
    LOG_INFO("DecoderController destroyed");
}

bool DecoderController::open(const std::string &url, const Config &config)
{
    LOG_INFO("Opening media synchronously: {}", url);
    LOG_DEBUG("Config - decodeMediaType: {}, enableAutoReconnect: {}, enableFrameRateControl: {}",
              static_cast<int>(config.decodeMediaType), config.enableAutoReconnect,
              config.enableFrameRateControl);

    // 取消任何正在进行的异步打开操作
    cancelAsyncOpen();

    // 停止任何正在进行的重连
    stopReconnect();

    std::lock_guard<std::mutex> lock(mutex_);
    const bool result = openInternal(url, config);

    if (result) {
        LOG_INFO("Successfully opened media: {}", url);
    } else {
        LOG_ERROR("Failed to open media: {}", url);
    }

    return result;
}

void DecoderController::openAsync(const std::string &url, const Config &config,
                                  AsyncOpenCallback callback)
{
    LOG_INFO("Opening media asynchronously: {}", url);
    LOG_DEBUG("Async open config - decodeMediaType: {}, enableAutoReconnect: {}",
              static_cast<int>(config.decodeMediaType), config.enableAutoReconnect);

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
    LOG_DEBUG("Async open operation started for: {}", url);

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
                LOG_WARN("Async open canceled before starting for: {}", url);
            } else {
                LOG_DEBUG("Executing async open for: {}", url);
                // 执行实际的打开操作
                openSuccess = openAsyncInternal(url, config);

                if (shouldCancelAsyncOpen_.load()) {
                    result = AsyncOpenResult::kCancelled;
                    errorMessage = "Operation was canceled during execution";
                    LOG_WARN("Async open canceled during execution for: {}", url);
                    // 如果打开成功但被取消，需要关闭
                    if (openSuccess) {
                        demuxer_->close();
                        openSuccess = false;
                    }
                } else if (openSuccess) {
                    result = AsyncOpenResult::kSuccess;
                    LOG_INFO("Async open completed successfully for: {}", url);
                } else {
                    result = AsyncOpenResult::kFailed;
                    errorMessage = "Failed to open media file";
                    LOG_ERROR("Async open failed for: {}", url);
                }
            }
        } catch (const std::exception &e) {
            result = AsyncOpenResult::kFailed;
            openSuccess = false;
            errorMessage = std::string("Exception occurred: ") + e.what();
            LOG_ERROR("Exception during async open for {}: {}", url, e.what());
        }

        // 调用回调函数
        AsyncOpenCallback callback;
        {
            std::lock_guard<std::mutex> lock(asyncCallbackMutex_);
            callback = asyncOpenCallback_;
            asyncOpenCallback_ = nullptr; // 清空回调
        }

        if (callback) {
            LOG_DEBUG("Calling async open callback for: {}", url);
            callback(result, openSuccess, errorMessage);
        }

        // 重置状态
        asyncOpenInProgress_.store(false);
        LOG_DEBUG("Async open operation completed for: {}", url);
    });
}

void DecoderController::cancelAsyncOpen()
{
    if (asyncOpenInProgress_.load()) {
        LOG_INFO("Canceling async open operation");

        // 设置取消标志
        shouldCancelAsyncOpen_.store(true);

        // 等待异步操作完成（这会触发回调）
        if (asyncOpenFuture_.valid()) {
            try {
                asyncOpenFuture_.wait();
                LOG_DEBUG("Async open operation canceled successfully");
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
    LOG_INFO("Closing decoder controller, url: {}", originalUrl_);

    // 停止重连
    stopReconnect();

    // 取消任何正在进行的异步打开操作
    cancelAsyncOpen();

    // 清理预缓冲状态
    cleanupPreBufferState();

    // 停止解码
    stopDecode();

    std::lock_guard<std::mutex> lock(mutex_);
    const bool result = closeInternal();

    if (result) {
        LOG_INFO("Decoder controller closed successfully");
    } else {
        LOG_ERROR("Failed to close decoder controller");
    }

    return result;
}

bool DecoderController::pause()
{
    LOG_DEBUG("Pausing decoder controller");
    std::lock_guard<std::mutex> lock(mutex_);

    if (!demuxer_) {
        LOG_ERROR("Cannot pause: demuxer is null");
        return false;
    }

    if (videoDecoder_) {
        videoDecoder_->pause();
        LOG_DEBUG("Video decoder paused");
    }
    if (audioDecoder_) {
        audioDecoder_->pause();
        LOG_DEBUG("Audio decoder paused");
    }

    isDemuxerPausedWhenReconnected_.store(true);
    const bool result = demuxer_->pause();

    if (result) {
        LOG_INFO("Decoder controller paused successfully");
    } else {
        LOG_ERROR("Failed to pause decoder controller");
    }

    return result;
}

bool DecoderController::resume()
{
    LOG_DEBUG("Resuming decoder controller");
    std::lock_guard<std::mutex> lock(mutex_);

    if (!demuxer_) {
        LOG_ERROR("Cannot resume: demuxer is null");
        return false;
    }

    if (demuxer_->isRealTime()) {
        syncController_->resetClocks();
        LOG_DEBUG("Reset clocks for real-time stream");
    }

    if (videoDecoder_) {
        videoDecoder_->resume();
        LOG_DEBUG("Video decoder resumed");
    }
    if (audioDecoder_) {
        audioDecoder_->resume();
        LOG_DEBUG("Audio decoder resumed");
    }

    isDemuxerPausedWhenReconnected_.store(false);
    const bool result = demuxer_->resume();

    if (result) {
        LOG_INFO("Decoder controller resumed successfully");
    } else {
        LOG_ERROR("Failed to resume decoder controller");
    }

    return result;
}

bool DecoderController::startDecode()
{
    LOG_INFO("Starting decode");
    std::lock_guard<std::mutex> lock(mutex_);
    hasDecoderWhenReconnected_.store(true);
    const bool result = startDecodeInternal();

    if (result) {
        LOG_INFO("Decode started successfully");
    } else {
        LOG_ERROR("Failed to start decode");
    }

    return result;
}

bool DecoderController::stopDecode()
{
    LOG_INFO("Stopping decode");
    std::lock_guard<std::mutex> lock(mutex_);
    hasDecoderWhenReconnected_.store(false);
    const bool result = stopDecodeInternal();

    if (result) {
        LOG_INFO("Decode stopped successfully");
    } else {
        LOG_ERROR("Failed to stop decode");
    }

    return result;
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
    LOG_INFO("Seeking to position: {:.3f}s", position);
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
        LOG_ERROR("Cannot seek: demuxer is null");
        sendFailedEvent();
        return false;
    }

    // 如果是实时流，则不支持seek
    if (demuxer_->isRealTime()) {
        LOG_WARN("Seek not supported for real-time streams");
        sendFailedEvent();
        return false;
    }

    LOG_DEBUG("Executing seek operation to position: {:.3f}s", position);

    // 执行seek操作
    bool result = demuxer_->seek(position);

    if (result) {
        LOG_DEBUG("Seek operation successful, updating clocks and queues");

        // 先重置同步控制器的时钟
        syncController_->resetClocks();

        // 清空队列，并设置seek节点
        if (videoDecoder_) {
            videoDecoder_->frameQueue()->clear();
            videoDecoder_->setSeekPos(position);
            LOG_DEBUG("Video decoder queue cleared and seek position set");
        }
        if (audioDecoder_) {
            audioDecoder_->frameQueue()->clear();
            audioDecoder_->setSeekPos(position);
            LOG_DEBUG("Audio decoder queue cleared and seek position set");
        }

        // 重新初始化时钟基准
        if (audioDecoder_) {
            syncController_->updateAudioClock(position,
                                              demuxer_->packetQueue(AVMEDIA_TYPE_AUDIO)->serial());
            LOG_DEBUG("Audio clock updated for seek");
        }
        if (videoDecoder_) {
            syncController_->updateVideoClock(position,
                                              demuxer_->packetQueue(AVMEDIA_TYPE_VIDEO)->serial());
            LOG_DEBUG("Video clock updated for seek");
        }

        LOG_INFO("Seek completed successfully to position: {:.3f}s", position);
    } else {
        LOG_ERROR("Seek failed to position: {:.3f}s", position);
    }

    // 发送seek成功的事件
    event = std::make_shared<SeekEventArgs>(syncController_->getMasterClock(), position,
                                            "DecoderController", "Seek Success");
    eventDispatcher_->triggerEvent(EventType::kSeekSuccess, event);

    return result;
}

bool DecoderController::setSpeed(double speed)
{
    LOG_INFO("Setting playback speed to: {:.2f}x", speed);
    std::lock_guard<std::mutex> lock(mutex_);

    if (speed <= 0.0f) {
        LOG_ERROR("Invalid speed value: {:.2f}, must be greater than 0", speed);
        return false;
    }

    // 如果是实时流，则不支持设置速度
    if (!demuxer_ || demuxer_->isRealTime()) {
        LOG_WARN("Speed control not supported for real-time streams");
        return false;
    }

    config_.speed = speed;
    LOG_DEBUG("Speed configuration updated to: {:.2f}x", speed);

    // 设置解码器速度
    if (videoDecoder_) {
        videoDecoder_->setSpeed(speed);
        LOG_DEBUG("Video decoder speed set to: {:.2f}x", speed);
    }
    if (audioDecoder_) {
        audioDecoder_->setSpeed(speed);
        LOG_DEBUG("Audio decoder speed set to: {:.2f}x", speed);
    }

    // 设置时钟速度
    if (syncController_) {
        syncController_->setSpeed(speed);
        LOG_DEBUG("Sync controller speed set to: {:.2f}x", speed);
    }

    LOG_INFO("Playback speed successfully set to: {:.2f}x", speed);
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
    LOG_DEBUG("Setting master clock type to: {}", static_cast<int>(type));
    syncController_->setMaster(type);
    LOG_INFO("Master clock type set successfully");
}

double DecoderController::getVideoFrameRate() const
{
    const double frameRate = videoDecoder_ ? videoDecoder_->getFrameRate() : 0.0;
    LOG_TRACE("Video frame rate: {:.2f} fps", frameRate);
    return frameRate;
}

void DecoderController::setFrameRateControl(bool enable)
{
    LOG_INFO("Setting frame rate control: {}", enable ? "enabled" : "disabled");

    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_.enableFrameRateControl = enable;
    }

    if (videoDecoder_) {
        videoDecoder_->setFrameRateControl(enable);
        LOG_DEBUG("Video decoder frame rate control updated");
    }
}

bool DecoderController::isFrameRateControlEnabled() const
{
    const bool enabled = videoDecoder_ ? videoDecoder_->isFrameRateControlEnabled() : false;
    LOG_TRACE("Frame rate control enabled: {}", enabled);
    return enabled;
}

double DecoderController::curSpeed() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    LOG_TRACE("Current speed: {:.2f}x", config_.speed);
    return config_.speed;
}

bool DecoderController::startRecording(const std::string &outputPath)
{
    LOG_INFO("Starting recording to: {}", outputPath);

    if (!demuxer_) {
        LOG_ERROR("Cannot start recording: demuxer is null");
        return false;
    }

    const bool result = demuxer_->startRecording(outputPath);

    if (result) {
        LOG_INFO("Recording started successfully to: {}", outputPath);
    } else {
        LOG_ERROR("Failed to start recording to: {}", outputPath);
    }

    return result;
}

bool DecoderController::stopRecording()
{
    LOG_INFO("Stopping recording");

    if (!demuxer_) {
        LOG_ERROR("Cannot stop recording: demuxer is null");
        return false;
    }

    const bool result = demuxer_->stopRecording();

    if (result) {
        LOG_INFO("Recording stopped successfully");
    } else {
        LOG_ERROR("Failed to stop recording");
    }

    return result;
}

bool DecoderController::isRecording() const
{
    const bool recording = demuxer_ && demuxer_->isRecording();
    LOG_TRACE("Recording status: {}", recording ? "active" : "inactive");
    return recording;
}

GlobalEventListenerHandle DecoderController::addGlobalEventListener(
    const std::function<EventCallback> &callback)
{
    LOG_DEBUG("Adding global event listener");
    return eventDispatcher_->addGlobalEventListener(callback);
}

bool DecoderController::removeGlobalEventListener(const GlobalEventListenerHandle &handle)
{
    LOG_DEBUG("Removing global event listener");
    return eventDispatcher_->removeGlobalEventListener(handle);
}

EventListenerHandle DecoderController::addEventListener(
    EventType eventType, const std::function<EventCallback> &callback)
{
    LOG_DEBUG("Adding event listener for type: {}", static_cast<int>(eventType));
    return eventDispatcher_->addEventListener(eventType, callback);
}

bool DecoderController::removeEventListener(EventType eventType, EventListenerHandle handle)
{
    LOG_DEBUG("Removing event listener for type: {}", static_cast<int>(eventType));
    return eventDispatcher_->removeEventListener(eventType, handle);
}

bool DecoderController::processAsyncEvents()
{
    return eventDispatcher_->processAsyncEvents();
}

void DecoderController::startAsyncProcessing()
{
    LOG_DEBUG("Starting async event processing");
    eventDispatcher_->startAsyncProcessing();
}

void DecoderController::stopAsyncProcessing()
{
    LOG_DEBUG("Stopping async event processing");
    eventDispatcher_->stopAsyncProcessing();
}

bool DecoderController::isAsyncProcessingActive() const
{
    const bool active = eventDispatcher_->isAsyncProcessingActive();
    LOG_TRACE("Async processing active: {}", active);
    return active;
}

bool DecoderController::isRealTimeUrl() const
{
    const bool isRealTime = demuxer_ ? demuxer_->isRealTime() : false;
    LOG_TRACE("Is real-time URL: {}", isRealTime);
    return isRealTime;
}

bool DecoderController::setLoopMode(LoopMode mode, int maxLoops)
{
    LOG_INFO("Setting loop mode: {}, max loops: {}", static_cast<int>(mode), maxLoops);

    if (!demuxer_) {
        LOG_ERROR("Cannot set loop mode: demuxer is null");
        return false;
    }

    // 只有文件流才支持循环播放
    if (isRealTimeUrl()) {
        LOG_WARN("Loop mode is not supported for real-time streams");
        return false;
    }

    demuxer_->setLoopMode(mode, maxLoops);
    LOG_INFO("Loop mode set successfully");
    return true;
}

LoopMode DecoderController::getLoopMode() const
{
    if (!demuxer_) {
        LOG_TRACE("Getting loop mode: demuxer is null, returning kNone");
        return LoopMode::kNone;
    }

    const LoopMode mode = demuxer_->getLoopMode();
    LOG_TRACE("Current loop mode: {}", static_cast<int>(mode));
    return mode;
}

int DecoderController::getCurrentLoopCount() const
{
    if (!demuxer_) {
        LOG_TRACE("Getting loop count: demuxer is null, returning 0");
        return 0;
    }

    const int count = demuxer_->getCurrentLoopCount();
    LOG_TRACE("Current loop count: {}", count);
    return count;
}

bool DecoderController::resetLoopCount()
{
    LOG_DEBUG("Resetting loop count");

    if (!demuxer_) {
        LOG_ERROR("Cannot reset loop count: demuxer is null");
        return false;
    }

    demuxer_->resetLoopCount();
    LOG_INFO("Loop count reset successfully");
    return true;
}

bool DecoderController::isReconnecting() const
{
    const bool reconnecting = isReconnecting_.load();
    LOG_TRACE("Reconnecting status: {}", reconnecting);
    return reconnecting;
}

void DecoderController::stopReconnectManually()
{
    LOG_INFO("Manually stopping reconnect");
    stopReconnect();
}

PreBufferState DecoderController::getPreBufferState() const
{
    const PreBufferState state = preBufferState_;
    LOG_TRACE("Pre-buffer state: {}", static_cast<int>(state));
    return state;
}

PreBufferProgress DecoderController::getPreBufferProgress() const
{
    const PreBufferProgress progress =
        demuxer_ ? demuxer_->getPreBufferProgress() : PreBufferProgress();
    LOG_TRACE("Pre-buffer progress - video: {}/{}, audio: {}/{}", progress.videoBufferedFrames,
              progress.videoRequiredFrames, progress.audioBufferedPackets,
              progress.audioRequiredPackets);
    return progress;
}

bool DecoderController::openInternal(const std::string &url, const Config &config)
{
    LOG_DEBUG("Opening internal: {}", url);

    // 保存原始URL用于重连
    originalUrl_ = url;

    // 重置预加载状态
    preBufferState_ = PreBufferState::kDisabled;

    config_ = config;

    // 打开媒体文件，并启用解复用器
    if (!demuxer_->open(url, config, std::bind(&DecoderController::onPreBufferReady, this))) {
        LOG_ERROR("Failed to open demuxer for: {}", url);
        return false;
    }

    LOG_DEBUG("Demuxer opened successfully for: {}", url);

    // 根据配置初始化解码器
    if (demuxer_->hasVideo() && (config_.decodeMediaType & Config::DecodeMediaType::kVideo)) {
        videoDecoder_ = std::make_shared<VideoDecoder>(demuxer_, syncController_, eventDispatcher_);
        LOG_DEBUG("Video decoder created for: {}", url);
    }

    // 开启音频解码器
    if (demuxer_->hasAudio() && (config_.decodeMediaType & Config::DecodeMediaType::kAudio)) {
        audioDecoder_ = std::make_shared<AudioDecoder>(demuxer_, syncController_, eventDispatcher_);
        LOG_DEBUG("Audio decoder created for: {}", url);
    }

    LOG_DEBUG("Internal open completed successfully for: {}", url);
    return true;
}

bool DecoderController::openAsyncInternal(const std::string &url, const Config &config)
{
    LOG_DEBUG("Async internal open for: {}", url);

    // 检查是否需要取消
    if (shouldCancelAsyncOpen_.load()) {
        LOG_DEBUG("Async open canceled for: {}", url);
        return false;
    }

    return openInternal(url, config);
}

bool DecoderController::closeInternal()
{
    LOG_DEBUG("Closing internal");

    // 析构解码器
    if (videoDecoder_) {
        videoDecoder_.reset();
        LOG_DEBUG("Video decoder destroyed");
    }
    if (audioDecoder_) {
        audioDecoder_.reset();
        LOG_DEBUG("Audio decoder destroyed");
    }

    const bool result = demuxer_->close();

    if (result) {
        LOG_DEBUG("Internal close completed successfully");
    } else {
        LOG_ERROR("Failed to close demuxer");
    }

    return result;
}

bool DecoderController::startDecodeInternal()
{
    LOG_DEBUG("Starting decode internal");

    // 重置同步控制器
    syncController_->resetClocks();
    LOG_DEBUG("Sync controller clocks reset");

    // 开启视频解码器
    if (demuxer_->hasVideo() && (config_.decodeMediaType & Config::DecodeMediaType::kVideo) &&
        videoDecoder_) {
        videoDecoder_->init(config_);
        videoDecoder_->setFrameRateControl(config_.enableFrameRateControl);
        videoDecoder_->setSpeed(config_.speed);
        if (!videoDecoder_->open()) {
            LOG_ERROR("Failed to open video decoder");
            return false;
        }
        LOG_DEBUG("Video decoder initialized and opened");
    }

    // 开启音频解码器
    if (demuxer_->hasAudio() && (config_.decodeMediaType & Config::DecodeMediaType::kAudio) &&
        audioDecoder_) {
        audioDecoder_->init(config_);
        audioDecoder_->setSpeed(config_.speed);
        if (!audioDecoder_->open()) {
            LOG_ERROR("Failed to open audio decoder");
            return false;
        }
        LOG_DEBUG("Audio decoder initialized and opened");
    }

    // 默认使用音频作为主时钟
    if (demuxer_->hasAudio() && audioDecoder_) {
        syncController_->setMaster(MasterClock::kAudio);
        LOG_DEBUG("Master clock set to audio");
    } else if (demuxer_->hasVideo() && videoDecoder_) {
        syncController_->setMaster(MasterClock::kVideo);
        LOG_DEBUG("Master clock set to video");
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
        LOG_DEBUG("Video decoder started");
    }

    if (audioDecoder_) {
        audioDecoder_->start();
        LOG_DEBUG("Audio decoder started");
    }

    isDecoding_ = true;
    LOG_DEBUG("Decode internal started successfully");

    return true;
}

bool DecoderController::stopDecodeInternal()
{
    LOG_DEBUG("Stopping decode internal");

    // 停止解码器
    if (videoDecoder_) {
        videoDecoder_->stop();
        videoDecoder_->close();
        LOG_DEBUG("Video decoder stopped and closed");
    }

    if (audioDecoder_) {
        audioDecoder_->stop();
        audioDecoder_->close();
        LOG_DEBUG("Audio decoder stopped and closed");
    }

    isDecoding_ = false;
    LOG_DEBUG("Decode internal stopped successfully");

    return true;
}

void DecoderController::onPreBufferReady()
{
    LOG_DEBUG("Pre-buffer ready callback triggered");
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
    LOG_DEBUG("Cleaning up pre-buffer state");
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

    LOG_DEBUG("Pre-buffer state cleaned up");
}

void DecoderController::startReconnect()
{
    LOG_DEBUG("Starting reconnect process");
    std::lock_guard<std::mutex> lock(mutex_);

    // 如果已经在重连中，则不重复启动
    if (isReconnecting_.load()) {
        LOG_DEBUG("Reconnect already in progress, skipping");
        return;
    }

    // 如果重连被禁用，则不启动重连
    if (!config_.enableAutoReconnect) {
        LOG_DEBUG("Auto reconnect disabled, skipping");
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
    LOG_DEBUG("Stopping reconnect process");
    std::unique_lock<std::mutex> lock(mutex_);

    if (!isReconnecting_.load()) {
        LOG_DEBUG("No reconnect in progress, skipping stop");
        return;
    }

    // 设置停止标志
    shouldStopReconnect_.store(true);

    // 等待重连线程结束
    if (reconnectThread_.joinable()) {
        lock.unlock();
        LOG_DEBUG("Waiting for reconnect thread to join");
        reconnectThread_.join();
        lock.lock();
    }

    cleanupReconnectState();
    isReconnecting_.store(false);
    LOG_INFO("Reconnect stopped for URL: {}", originalUrl_);
}

void DecoderController::reconnectLoop()
{
    LOG_DEBUG("Reconnect loop started");

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
        LOG_DEBUG("Waiting {}ms before next reconnect attempt", config_.reconnectIntervalMs);
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
    LOG_DEBUG("Reconnect loop ended");
}

bool DecoderController::attemptReconnect()
{
    LOG_DEBUG("Attempting single reconnect for: {}", originalUrl_);

    try {
        // 先关闭当前连接
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // 停止解码
            stopDecodeInternal();

            // 关闭解复用器
            if (!demuxer_->close()) {
                LOG_DEBUG("Failed to close {} demuxer during reconnect", originalUrl_);
                return false;
            }
        }

        // 短暂等待，确保资源完全释放
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 检查是否应该停止重连
        if (shouldStopReconnect_.load()) {
            LOG_DEBUG("Reconnect stop requested, aborting attempt");
            return false;
        }

        // 尝试重新打开
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!demuxer_->open(originalUrl_, config_,
                                std::bind(&DecoderController::onPreBufferReady, this))) {
                LOG_DEBUG("Failed to reopen demuxer during reconnect");
                return false;
            }

            // 如果之前的demuxer是暂停状态，则重新暂停
            if (isDemuxerPausedWhenReconnected_.load()) {
                demuxer_->pause();
                LOG_DEBUG("Demuxer paused state restored during reconnect");
            }

            // 如果重连之前，存在解码器，则开启
            if (hasDecoderWhenReconnected_.load()) {
                if (!startDecodeInternal()) {
                    LOG_ERROR("Failed to restart decoder during reconnect");
                    return false;
                }
                LOG_DEBUG("Decoder restarted during reconnect");
            }
        }

        LOG_DEBUG("Reconnect attempt successful");
        return true;
    } catch (const std::exception &e) {
        LOG_ERROR("Exception during reconnect attempt: {}", e.what());
        return false;
    }
}

void DecoderController::cleanupReconnectState()
{
    LOG_DEBUG("Cleaning up reconnect state");
    isReconnecting_.store(false);
    shouldStopReconnect_.store(false);
    currentReconnectAttempt_.store(0);
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END