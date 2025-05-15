#pragma once
#include "AudioDecoder.h"
#include "Logger.h"
#include "SyncController.h"
#include "VideoDecoder.h"
#include <memory>

class DecoderManager {
public:
    struct Config {
        bool enableFrameRateControl = true;
    };

public:
    DecoderManager();
    ~DecoderManager();
    
    // 打开媒体文件
    bool open(const std::string& filePath, const Config &config = Config());
    bool close();

    bool pause();
    bool resume();

    bool startDecode();
    bool stopDecode();

    bool seek(double position);
    bool setSpeed(double speed);
    
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

    double curSpeed() const;

private:
    std::shared_ptr<Demuxer> demuxer_;
    std::shared_ptr<VideoDecoder> videoDecoder_;
    std::shared_ptr<AudioDecoder> audioDecoder_;
    std::shared_ptr<SyncController> syncController_;

    // 当前播放速度
    double speed_ = 1.0;

    // 当前是否已开始解码
    bool isStartDecoding_ = false;

    // 解码器配置项
    Config config_;
};