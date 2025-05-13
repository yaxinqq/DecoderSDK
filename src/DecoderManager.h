#pragma once
#include "VideoDecoder.h"
#include "AudioDecoder.h"
#include "SyncController.h"
#include <memory>

class DecoderManager {
public:
    DecoderManager();
    ~DecoderManager();
    
    // 打开媒体文件
    bool open(const std::string& filePath);
    
    // 开始解码
    void start();
    
    // 停止解码
    void stop();
    
    // 获取视频帧队列
    FrameQueue& videoQueue();
    
    // 获取音频帧队列
    FrameQueue& audioQueue();
    
    // 设置主时钟类型
    void setMasterClock(SyncController::MasterClock type);
    
    // 获取视频帧率
    double getVideoFrameRate() const;
    
    // 设置是否启用帧率控制
    void setFrameRateControl(bool enable);
    
    // 获取是否启用帧率控制
    bool isFrameRateControlEnabled() const;

private:
    std::shared_ptr<Demuxer> demuxer_;
    std::shared_ptr<VideoDecoder> videoDecoder_;
    std::shared_ptr<AudioDecoder> audioDecoder_;
    SyncController syncController_;
};