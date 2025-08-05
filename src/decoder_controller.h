#ifndef DECODER_SDK_INTERNAL_CONTROLLER_H
#define DECODER_SDK_INTERNAL_CONTROLLER_H
#include <atomic>
#include <future>
#include <memory>

#include "base/base_define.h"
#include "decoder/audio_decoder.h"
#include "decoder/video_decoder.h"
#include "demuxer/demuxer.h"
#include "event_system/event_dispatcher.h"
#include "include/decodersdk/common_define.h"
#include "stream_sync/stream_sync_manager.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class DecoderController {
public:
    /**
     * @brief 构造函数
     */
    DecoderController();
    /**
     * @brief 析构函数
     */
    ~DecoderController();

    /**
     * @brief 同步打开媒体
     *
     * @param url 媒体路径
     * @param config 配置参数
     * @return true 成功
     * @return false 失败
     */
    bool open(const std::string &url, const Config &config = Config());

    /**
     * @brief 异步打开媒体
     *
     * @param url 媒体路径
     * @param config 配置参数
     * @param callback 回调函数
     */
    void openAsync(const std::string &url, const Config &config, AsyncOpenCallback callback);

    /**
     * @brief 取消异步打开操作
     */
    void cancelAsyncOpen();

    /**
     * @brief 检查是否有异步打开操作正在进行
     *
     * @return true 正在进行; false 未进行
     */
    bool isAsyncOpenInProgress() const;

    /**
     * @brief 关闭解码器
     *
     * @return true 成功,false 失败
     */
    bool close();

    /**
     * @brief 暂停解码
     *
     * @return true 成功; false 失败
     */
    bool pause();
    /**
     * @brief 恢复解码
     *
     * @return true 成功; false 失败
     */
    bool resume();

    /**
     * @brief 开始解码
     *
     * @return true 成功; false 失败
     */
    bool startDecode();
    /**
     * @brief 停止解码
     *
     * @return true 成功; false 失败
     */
    bool stopDecode();
    /**
     * @brief 解码是否已停止
     *
     * @return true 已停止; false 未停止
     */
    bool isDecodeStopped() const;
    /**
     * @brief 是否已暂停
     *
     * @return true 已停止; false 未停止
     */
    bool isPaused() const;

    /**
     * @brief 定位
     *
     * @param position 定位时间位置
     * @return true 成功; false 失败
     */
    bool seek(double position);
    /**
     * @brief 设置播放速度
     *
     * @param speed 播放速度
     * @return true 成功; false 失败
     */
    bool setSpeed(double speed);

    /**
     * @brief 获取视频帧队列
     *
     * @return 视频帧队列
     */
    std::shared_ptr<FrameQueue> videoQueue();

    /**
     * @brief 获取音频帧队列
     *
     * @return 音频帧队列
     */
    std::shared_ptr<FrameQueue> audioQueue();

    /**
     * @brief 设置主时钟类型
     *
     * @param type 主时钟类型
     */
    void setMasterClock(MasterClock type);

    /**
     * @brief 获取视频帧率
     *
     * @return 视频帧率
     */
    double getVideoFrameRate() const;

    /**
     * @brief 设置是否启用帧率控制
     *
     * @param enable true 启用; false 禁用
     */
    void setFrameRateControl(bool enable);

    /**
     * @brief 获取是否启用帧率控制
     *
     * @return true 启用; false 禁用
     */
    bool isFrameRateControlEnabled() const;

    /**
     * @brief 获取当前播放速度
     *
     * @return 播放速度
     */
    double curSpeed() const;

    /**
     * @brief 开始录像，暂时将输出文件保存为.mp4格式
     *
     * @param outputPath 输出路径
     * @return true 成功; false 失败
     */
    bool startRecording(const std::string &outputPath);
    /**
     * @brief 停止录像
     *
     * @return true 成功; false 失败
     */
    bool stopRecording();
    /**
     * @brief 获取是否正在录像
     *
     * @return true 正在录像; false 未录像
     */
    bool isRecording() const;

    /**
     * @brief 设置全部事件的监听器
     *
     * @param callback 回调函数
     * @return 全局事件监听器句柄
     */
    GlobalEventListenerHandle addGlobalEventListener(const std::function<EventCallback> &callback);
    /**
     * @brief 移除全局事件监听器
     *
     * @param handle 全局事件监听器句柄
     * @return true 成功; false 失败
     */
    bool removeGlobalEventListener(const GlobalEventListenerHandle &handle);
    /**
     * @brief 添加事件监听器
     *
     * @param eventType 事件类型
     * @param callback 回调函数
     * @return 事件监听器句柄
     */
    EventListenerHandle addEventListener(EventType eventType,
                                         const std::function<EventCallback> &callback);
    /**
     * @brief 移除事件监听器
     *
     * @param eventType 事件类型
     * @param handle 事件监听器句柄
     * @return true 成功; false 失败
     */
    bool removeEventListener(EventType eventType, EventListenerHandle handle);
    /**
     * @brief 异步事件处理
     *
     * @return true 成功; false 失败
     */
    bool processAsyncEvents();
    /**
     * @brief 启动异步事件处理线程
     */
    void startAsyncProcessing();
    /**
     * @brief 停止异步事件处理线程
     */
    void stopAsyncProcessing();

    /**
     * @brief 检查异步事件处理线程是否正在运行
     *
     * @return true 正在运行; false 已停止
     */
    bool isAsyncProcessingActive() const;

    /**
     * @brief 检查是否为实时流地址
     *
     * @return true 是实时流地址; false 不是实时流地址
     */
    bool isRealTimeUrl() const;

    /**
     * @brief 设置循环播放模式
     * @param mode 循环模式
     * @param maxLoops 最大循环次数（仅在kSingle模式下有效，-1表示无限循环）
     * @return true 成功; false 失败
     */
    bool setLoopMode(LoopMode mode, int maxLoops = -1);

    /**
     * @brief 获取循环播放模式
     * @return 当前循环模式
     */
    LoopMode getLoopMode() const;

    /**
     * @brief 获取当前循环次数
     * @return 当前循环次数
     */
    int getCurrentLoopCount() const;

    /**
     * @brief 重置循环计数
     * @return true 成功; false 失败
     */
    bool resetLoopCount();

    /**
     * @brief 检查是否正在重连
     * @return true 正在重连; false 未重连
     */
    bool isReconnecting() const;

    /**
     * @brief 手动停止重连
     */
    void stopReconnectManually();

    /**
     * @brief 获取预缓冲状态
     *
     * @return PreBufferState 预缓冲状态
     */
    PreBufferState getPreBufferState() const;
    /**
     * @brief 获取预缓冲进度
     *
     * @return PreBufferProgress 预缓冲进度
     */
    PreBufferProgress getPreBufferProgress() const;

private:
    /**
     * @brief 同步打开的内部实现，不加锁
     *
     * @param url 媒体文件URL
     * @param config 配置项
     * @return true 成功; false 失败
     */
    bool openInternal(const std::string &url, const Config &config);
    /**
     * @brief 异步打开的内部实现，不加锁
     *
     * @param url 媒体文件URL
     * @param config 配置项
     * @return true 成功; false 失败
     */
    bool openAsyncInternal(const std::string &url, const Config &config);
    /**
     * @brief 关闭的内部实现，不加锁
     *
     * @return true 成功; false 失败
     */
    bool closeInternal();

    /**
     * @brief 内部开始解码
     *
     * @return true 成功; false 失败
     */
    bool startDecodeInternal();
    /**
     * @brief 内部停止解码
     *
     * @return true 成功; false 失败
     */
    bool stopDecodeInternal();

    /**
     * @brief 预缓冲结束后的回调
     */
    void onPreBufferReady();
    // 清理预缓冲状态
    void cleanupPreBufferState();

    /**
     * @brief 开始重连流程
     */
    void startReconnect();

    /**
     * @brief 停止重连流程
     */
    void stopReconnect();

    /**
     * @brief 重连线程主循环
     */
    void reconnectLoop();

    /**
     * @brief 执行单次重连尝试
     * @return true 重连成功; false 重连失败
     */
    bool attemptReconnect();

    /**
     * @brief 清理重连状态
     */
    void cleanupReconnectState();

private:
    std::shared_ptr<EventDispatcher> eventDispatcher_;  // 事件分发器
    std::shared_ptr<StreamSyncManager> syncController_; // 流同步管理器

    std::shared_ptr<Demuxer> demuxer_;           // 解复用器
    std::shared_ptr<VideoDecoder> videoDecoder_; // 视频解码器
    std::shared_ptr<AudioDecoder> audioDecoder_; // 音频解码器

    Config config_;          // 解码器配置项
    bool isDecoding_{false}; // 是否正在解码
    bool isPaused_{false};   // 是否已暂停

    // 预缓冲状态
    std::atomic<PreBufferState> preBufferState_{PreBufferState::kDisabled};

    std::atomic<bool> asyncOpenInProgress_{false};   // 异步打开操作是否进行中
    std::atomic<bool> shouldCancelAsyncOpen_{false}; // 是否应该取消异步打开操作
    std::future<void> asyncOpenFuture_;              // 异步打开操作的future对象
    AsyncOpenCallback asyncOpenCallback_;            // 异步打开回调函数
    std::mutex asyncCallbackMutex_;                  // 异步回调函数的互斥锁

    mutable std::mutex mutex_; // 互斥锁，保护内部状态

    // 重连相关成员变量
    std::atomic<bool> isReconnecting_{false};                 // 是否正在重连
    std::atomic<bool> shouldStopReconnect_{false};            // 是否应该停止重连
    std::atomic<int> currentReconnectAttempt_{0};             // 当前重连次数
    std::atomic<bool> hasDecoderWhenReconnected_{false};      // 记录重连时是否有解码器
    std::atomic<bool> isDemuxerPausedWhenReconnected_{false}; // 记录重连时解复用器是否暂停
    std::string originalUrl_;                                 // 原始URL，用于重连
    std::thread reconnectThread_;                             // 重连线程
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_CONTROLLER_H