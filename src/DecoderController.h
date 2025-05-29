#pragma once
#include "AudioDecoder.h"
#include "Logger.h"
#include "SyncController.h"
#include "VideoDecoder.h"
#include <memory>

class DecoderController {
public:
    struct Config {
        // 是否开启帧率控制
        bool enableFrameRateControl = true;
        // 播放速度
        double speed = 1.0;
        // 硬件解码器类型
        HWAccelType hwAccelType = HWAccelType::AUTO;
        // 硬件解码器设备索引
        int hwDeviceIndex = 0;
        // 软解时的视频输出格式
        AVPixelFormat videoOutFormat = AV_PIX_FMT_YUV420P;
        // 需要解码后的帧位于内存中
        bool requireFrameInSystemMemory = false;
    };

public:
    DecoderController();
    ~DecoderController();

    // 打开媒体文件
    bool open(const std::string &filePath, const Config &config = Config());
    bool close();

    bool pause();
    bool resume();

    bool startDecode();
    bool stopDecode();

    bool seek(double position);
    bool setSpeed(double speed);

    // 获取视频帧队列
    FrameQueue &videoQueue();

    // 获取音频帧队列
    FrameQueue &audioQueue();

    // 设置主时钟类型
    void setMasterClock(SyncController::MasterClock type);

    // 获取视频帧率
    double getVideoFrameRate() const;

    // 设置是否启用帧率控制
    void setFrameRateControl(bool enable);

    // 获取是否启用帧率控制
    bool isFrameRateControlEnabled() const;

    // 当前播放速度
    double curSpeed() const;

    // 开始录像，暂时将输出文件保存为.mp4格式
    bool startRecording(const std::string &outputPath);
    // 停止录像
    bool stopRecording();
    // 是否正在录像
    bool isRecording() const;

private:
    // 录像线程
    void recordingLoop();

private:
    std::shared_ptr<Demuxer> demuxer_;
    std::shared_ptr<VideoDecoder> videoDecoder_;
    std::shared_ptr<AudioDecoder> audioDecoder_;
    std::shared_ptr<SyncController> syncController_;

    // 当前是否已开始解码
    bool isStartDecoding_ = false;

    // 解码器配置项
    Config config_;
    // 是否是实时流
    bool isLiveStream_ = false;

    // 录制相关
    AVFormatContext *recordFormatCtx_ = nullptr;
    std::string recordFilePath_;
    std::atomic_bool isRecording_ = false;
    std::thread recordThread_;
    std::mutex recordMutex_;
    std::condition_variable recordCv_;
    std::atomic_bool recordStopFlag_ = false;
};