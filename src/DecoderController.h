#pragma once
#include <memory>

#include "AudioDecoder.h"
#include "EventDispatcher.h"
#include "Logger.h"
#include "SyncController.h"
#include "VideoDecoder.h"

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

        // 重连配置
        bool enableAutoReconnect = true;  // 是否启用自动重连
        int maxReconnectAttempts = -1;    // 最大重连次数
        int reconnectIntervalMs = 1000;   // 重连间隔(毫秒)

        // 预缓冲配置
        struct PreBufferConfig {
            // 是否启用预缓冲
            bool enablePreBuffer = false;
            // 视频预缓冲帧数
            int videoPreBufferFrames = 0;
            // 音频预缓冲包数
            int audioPreBufferPackets = 0;
            // 是否需要音视频都达到预缓冲才开始
            bool requireBothStreams = false;
            // 预缓冲完成后是否自动开始解码
            bool autoStartAfterPreBuffer = true;
        } preBufferConfig;
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

    // 监听器相关
    // 设置全部事件的监听器
    GlobalEventListenerHandle addGlobalEventListener(EventCallback callback);

    // 移除全局事件监听器
    bool removeGlobalEventListener(const GlobalEventListenerHandle &handle);

    // 添加事件监听器
    EventListenerHandle addEventListener(EventType eventType,
                                         EventCallback callback);

    // 移除事件监听器
    bool removeEventListener(EventType eventType, EventListenerHandle handle);

    // 异步事件处理
    void processAsyncEvents();    // 主线程调用poll
    void startAsyncProcessing();  // 启动后台线程
    void stopAsyncProcessing();   // 停止后台线程

    // 状态查询
    bool isAsyncProcessingActive() const;
    // 获得所有事件类型
    std::vector<EventType> allEventTypes() const;
    // 获得事件名称(枚举定义)
    std::string getEventTypeName(EventType type) const;

    // 停止重连任务
    void stopReconnect();

    // 检查是否正在重连
    bool isReconnecting() const;

public:
    // 预缓冲状态
    enum class PreBufferState {
        Disabled,       // 未启用预缓冲
        WaitingBuffer,  // 等待预缓冲完成
        Ready,          // 预缓冲完成，正在解码
    };

    // 获取预缓冲状态
    PreBufferState getPreBufferState() const;

    // 获取预缓冲进度
    struct PreBufferProgress {
        int videoBufferedFrames;
        int audioBufferedPackets;
        int videoRequiredFrames;
        int audioRequiredPackets;
        double progressPercent;  // 0.0-1.0
        bool isVideoReady;
        bool isAudioReady;
        bool isOverallReady;
    };
    PreBufferProgress getPreBufferProgress() const;

private:
    // 流读取错误后，重新打开
    bool reopen(const std::string &url);
    // 处理重连
    void handleReconnect(const std::string &url);

    bool startDecodeInternal(bool reopen);
    bool stopDecodeInternal(bool reopen);

    // 预缓冲结束后的回调
    void onPreBufferReady();
    // 清理预缓冲状态
    void cleanupPreBufferState();

private:
    // 事件分发
    std::shared_ptr<EventDispatcher> eventDispatcher_;

    std::shared_ptr<Demuxer> demuxer_;
    std::shared_ptr<VideoDecoder> videoDecoder_;
    std::shared_ptr<AudioDecoder> audioDecoder_;
    std::shared_ptr<SyncController> syncController_;

    // 当前是否已开始解码
    bool isStartDecoding_ = false;

    // 解码器配置项
    Config config_;

    // 重连相关
    std::atomic<int> reconnectAttempts_{0};
    std::atomic_bool isReconnecting_{false};
    std::atomic_bool shouldStopReconnect_{false};  // 添加重连停止标志

    // 预缓冲状态
    std::atomic<PreBufferState> preBufferState_{PreBufferState::Disabled};
};
