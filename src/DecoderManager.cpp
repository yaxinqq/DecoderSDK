#include "DecoderManager.h"
#include "Logger.h"

DecoderManager::DecoderManager()
    : demuxer_(std::make_shared<Demuxer>())
    , syncController_(std::make_shared<SyncController>())
{
    Logger::initFromConfig("./etc/decoderSDK.json");
}

DecoderManager::~DecoderManager()
{
    stopDecode();
    close();
}

bool DecoderManager::open(const std::string& filePath, const Config &config)
{
    config_ = config;

    // 打开媒体文件
    if (!demuxer_->open(filePath)) {
        return false;
    }

    // 启动解复用器
    demuxer_->start();
    return true;
}

bool DecoderManager::close()
{
    // 停止解复用器
    if (demuxer_) {
        demuxer_->stop();
        demuxer_->close();
    }

    return true;
}

bool DecoderManager::pause()
{
    if (!demuxer_) {
        return false;
    }

    return demuxer_->pause();
}

bool DecoderManager::resume()
{
    if (!demuxer_) {
        return false;
    }

    return demuxer_->resume();
}

bool DecoderManager::startDecode()
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
        videoDecoder_->setFrameRateControl(config_.enableFrameRateControl);
        videoDecoder_->setSpeed(speed_);
        if (!videoDecoder_->open()) {
            return false;
        }
    }
    
    // 创建音频解码器
    if (demuxer_->hasAudio()) {
        audioDecoder_ = std::make_shared<AudioDecoder>(demuxer_, syncController_);
        audioDecoder_->setSpeed(speed_);
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

    return true;
}

bool DecoderManager::stopDecode()
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

    return true;
}

bool DecoderManager::seek(double position)
{
    if (!demuxer_) {
        return false;
    }

    return demuxer_->seek(position);
}

bool DecoderManager::setSpeed(double speed)
{
    if (speed <= 0.0f) {
        return false;
    }

    speed_ = speed;

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

FrameQueue& DecoderManager::videoQueue()
{
    static FrameQueue emptyQueue;
    return videoDecoder_ ? videoDecoder_->frameQueue() : emptyQueue;
}

FrameQueue& DecoderManager::audioQueue()
{
    static FrameQueue emptyQueue;
    return audioDecoder_ ? audioDecoder_->frameQueue() : emptyQueue;
}

void DecoderManager::setMasterClock(SyncController::MasterClock type)
{
    syncController_->setMaster(type);
}

double DecoderManager::getVideoFrameRate() const
{
    return videoDecoder_ ? videoDecoder_->getFrameRate() : 0.0;
}

void DecoderManager::setFrameRateControl(bool enable)
{
    if (videoDecoder_) {
        videoDecoder_->setFrameRateControl(enable);
    }
    config_.enableFrameRateControl = enable;
}

bool DecoderManager::isFrameRateControlEnabled() const
{
    return videoDecoder_ ? videoDecoder_->isFrameRateControlEnabled() : false;
}

double DecoderManager::curSpeed() const
{
    return speed_;
}