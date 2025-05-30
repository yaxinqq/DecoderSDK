#include "DecoderController.h"
#include "Logger.h"
#include "Utils.h"

DecoderController::DecoderController()
    : eventDispatcher_(std::make_shared<EventDispatcher>()),
      demuxer_(std::make_shared<Demuxer>(eventDispatcher_)),
      syncController_(std::make_shared<SyncController>())
{
}

DecoderController::~DecoderController()
{
    stopDecode();
    close();
}

bool DecoderController::open(const std::string &filePath, const Config &config)
{
    config_ = config;

    // 打开媒体文件，并启用解复用器
    if (!demuxer_->open(filePath, utils::isRealtime(filePath))) {
        return false;
    }

    return true;
}

bool DecoderController::close()
{
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
    // 如果当前已开始解码，则先停止
    if (isStartDecoding_) {
        stopDecode();
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

void DecoderController::removeAllGlobalListeners()
{
    eventDispatcher_->removeAllGlobalListeners();
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

// 移除事件监听器
bool DecoderController::removeEventListener(EventType eventType,
                                            EventListenerHandle handle)
{
    return eventDispatcher_->removeEventListener(eventType, handle);
}

// 移除指定事件类型的所有监听器
void DecoderController::removeAllListeners(EventType eventType)
{
    eventDispatcher_->removeAllListeners(eventType);
}

// 移除所有监听器
void DecoderController::removeAllListeners()
{
    eventDispatcher_->removeAllListeners();
}

// 启用/禁用异步事件处理
void DecoderController::setAsyncProcessing(bool enabled)
{
    eventDispatcher_->setAsyncProcessing(enabled);
}