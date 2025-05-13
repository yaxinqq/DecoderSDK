#include "DecoderManager.h"

DecoderManager::DecoderManager()
    : demuxer_(std::make_shared<Demuxer>())
{
}

DecoderManager::~DecoderManager()
{
    stop();
}

bool DecoderManager::open(const std::string& filePath)
{
    // 打开媒体文件
    if (!demuxer_->open(filePath)) {
        return false;
    }
    
    // 创建视频解码器
    if (demuxer_->hasVideo()) {
        videoDecoder_ = std::make_shared<VideoDecoder>(demuxer_);
        if (!videoDecoder_->open()) {
            return false;
        }
        
        // 设置视频时钟
        syncController_.setVideoClock(videoDecoder_->getClock());
    }
    
    // 创建音频解码器
    if (demuxer_->hasAudio()) {
        audioDecoder_ = std::make_shared<AudioDecoder>(demuxer_);
        if (!audioDecoder_->open()) {
            return false;
        }
        
        // 设置音频时钟
        syncController_.setAudioClock(audioDecoder_->getClock());
    }
    
    // 默认使用音频作为主时钟
    if (demuxer_->hasAudio()) {
        syncController_.setMasterClockType(SyncController::MasterClock::Audio);
    } else if (demuxer_->hasVideo()) {
        syncController_.setMasterClockType(SyncController::MasterClock::Video);
    }
    
    return true;
}

void DecoderManager::start()
{
    // 重置同步控制器
    syncController_.resetClocks();
    
    // 启动解码器
    if (videoDecoder_) {
        videoDecoder_->start();
    }
    
    if (audioDecoder_) {
        audioDecoder_->start();
    }
    
    // 启动解复用器
    demuxer_->start();
}

void DecoderManager::stop()
{
    // 停止解复用器
    if (demuxer_) {
        demuxer_->stop();
    }
    
    // 停止解码器
    if (videoDecoder_) {
        videoDecoder_->stop();
    }
    
    if (audioDecoder_) {
        audioDecoder_->stop();
    }
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
    syncController_.setMasterClockType(type);
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
}

bool DecoderManager::isFrameRateControlEnabled() const
{
    return videoDecoder_ ? videoDecoder_->isFrameRateControlEnabled() : false;
}