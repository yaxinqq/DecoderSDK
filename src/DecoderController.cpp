#include "DecoderController.h"
#include "Logger.h"
#include "Utils.h"

DecoderController::DecoderController()
    : demuxer_(std::make_shared<Demuxer>())
    , syncController_(std::make_shared<SyncController>())
{
    Logger::initFromConfig("./etc/decoderSDK.json");
}

DecoderController::~DecoderController()
{
    stopDecode();
    close();
}

bool DecoderController::open(const std::string& filePath, const Config &config)
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
        videoDecoder_ = std::make_shared<VideoDecoder>(demuxer_, syncController_);
        videoDecoder_->init(config_.hwAccelType, config_.hwDeviceIndex, config_.videoOutFormat);
        videoDecoder_->setFrameRateControl(config_.enableFrameRateControl);
        videoDecoder_->setSpeed(config_.speed);
        if (!videoDecoder_->open()) {
            return false;
        }
    }
    
    // 创建音频解码器
    if (demuxer_->hasAudio()) {
        audioDecoder_ = std::make_shared<AudioDecoder>(demuxer_, syncController_);
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