#pragma once

#include <eventpp/eventdispatcher.h>
#include <eventpp/eventqueue.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// 事件类型枚举
enum class EventType : uint32_t {
    // 流事件
    kStreamOpened = 1,    // 流已打开（调用open成功）
    kStreamClosed,        // 流已关闭（调用close成功）
    kStreamOpening,       // 流正在打开
    kStreamOpenFailed,    // 流打开失败
    kStreamClose,         // 关闭
    kStreamReadData,      // 读到第一帧数据
    kStreamReadError,     // 读取数据失败
    kStreamReadRecovery,  // 读取恢复
    kStreamEnded,         // 流结束

    // 解码相关事件
    kDecodeStarted = 20,  // 解码已开始（调用startDecode成功）
    kDecodeStopped,       // 解码已停止（调用stopDecode成功）
    kDecodePaused,  // 解码已暂停（调用pauseDecode成功）  // Todo: 暂时不支持
    kCreateDecoderSuccess,  // 创建解码器成功
    kCreateDecoderFailed,   // 创建解码器失败
    kDestoryDecoder,        // 销毁解码器
    kDecodeFirstFrame,      // 解出第一帧数据
    kDecodeError,           // 解码错误
    kDecodeRecovery,        // 解码恢复

    // seek相关事件
    kSeekStarted = 40,  // 开始seek,
    kSeekSuccess,       // seek成功
    kSeekFailed,        // seek失败

    // 录制相关事件
    kRecordingStarted = 60,  // 开始录制
    kRecordingStopped,       // 停止录制
    kRecordingError,         // 录制错误
};

enum class MediaType : uint8_t {
    kMediaTypeUnknown = 0,  // 未知
    kMediaTypeVideo,        // 视频
    kMediaTypeAudio,        // 音频
};

// 事件参数基类
class EventArgs {
public:
    EventArgs(const std::string &source = "",
              const std::string &description = "", int errcode = 0,
              const std::string &errorMessage = "")
        : source(source),
          description(description),
          errorCode(errcode),
          errorMessage(errorMessage)
    {
    }
    virtual ~EventArgs() = default;

    // 事件时间戳
    std::chrono::steady_clock::time_point timestamp =
        std::chrono::steady_clock::now();

    // 事件源标识
    std::string source;
    // 事件描述
    std::string description;
    // 错误码，0表示无错误
    int errorCode = 0;
    // 错误信息
    std::string errorMessage;
};

// 流事件参数
class StreamEventArgs : public EventArgs {
public:
    StreamEventArgs(const std::string &filePath = "",
                    const std::string &source = "",
                    const std::string &description = "", int errcode = 0,
                    const std::string &errorMessage = "")
        : EventArgs(source, description, errcode, errorMessage),
          filePath(filePath)
    {
    }

    std::string filePath;  // 文件路径
};

// 解码器事件参数
class DecoderEventArgs : public EventArgs {
public:
    DecoderEventArgs(const std::string &codecName = "", int streamIndex = -1,
                     MediaType mediaType = MediaType::kMediaTypeUnknown,
                     bool isHardwareAccel = false,
                     const std::string &source = "",
                     const std::string &description = "", int errcode = 0,
                     const std::string &errorMessage = "")
        : EventArgs(source, description, errcode, errorMessage),
          codecName(codecName),
          streamIndex(streamIndex),
          mediaType(mediaType),
          isHardwareAccel(isHardwareAccel)
    {
    }

    std::string codecName;         // 编解码器名称
    int streamIndex;               // 流索引
    MediaType mediaType;           // 媒体类型
    bool isHardwareAccel = false;  // 是否硬件加速
};

// Seek事件参数
class SeekEventArgs : public EventArgs {
public:
    SeekEventArgs(double position = 0.0, double targetPosition = 0.0,
                  const std::string &source = "",
                  const std::string &description = "", int errcode = 0,
                  const std::string &errorMessage = "")
        : EventArgs(source, description, errcode, errorMessage),
          position(position),
          targetPosition(targetPosition)
    {
    }

    double position;        // 当前位置（秒）
    double targetPosition;  // 目标位置（秒）
};

// 录制事件参数
class RecordingEventArgs : public EventArgs {
public:
    RecordingEventArgs(const std::string &outputPath = "",
                       const std::string &format = "",
                       const std::string &source = "",
                       const std::string &description = "", int errcode = 0,
                       const std::string &errorMessage = "")
        : EventArgs(source, description, errcode, errorMessage),
          outputPath(outputPath),
          format(format)
    {
    }

    std::string outputPath;  // 输出文件路径
    std::string format;      // 录制格式
};

// 连接类型（保留用于向后兼容，但不再影响监听器注册）
enum class ConnectionType {
    kDirect,  // 同步调用
    kQueued,  // 异步队列
    kAuto     // 自动选择
};

// 事件回调类型 - 必须匹配eventpp的函数签名格式
using EventCallback = void(EventType, std::shared_ptr<EventArgs>);

// 事件策略配置
struct EventPolicy {
    using Threading = eventpp::MultipleThreading;
};

// 基于eventpp的事件分发器类型定义
using SyncEventDispatcher =
    eventpp::EventDispatcher<EventType, EventCallback, EventPolicy>;
using AsyncEventQueue =
    eventpp::EventQueue<EventType, EventCallback, EventPolicy>;
using EventListenerHandle = SyncEventDispatcher::Handle;
using GlobalEventListenerHandle =
    std::unordered_map<EventType, SyncEventDispatcher::Handle>;

/**
 * 统一的事件分发器
 * 支持同步和异步事件分发，基于eventpp库
 * 监听器统一注册，事件触发时动态选择分发方式
 */
class EventDispatcher {
public:
    EventDispatcher();
    ~EventDispatcher();

    // 禁用拷贝和移动
    EventDispatcher(const EventDispatcher &) = delete;
    EventDispatcher &operator=(const EventDispatcher &) = delete;
    EventDispatcher(EventDispatcher &&) = delete;
    EventDispatcher &operator=(EventDispatcher &&) = delete;

    // 监听器管理 - 统一注册，不再区分连接类型
    EventListenerHandle addEventListener(
        EventType eventType, const std::function<EventCallback> &callback);

    bool removeEventListener(EventType eventType, EventListenerHandle handle);

    // 设置全部事件的监听器
    GlobalEventListenerHandle addGlobalEventListener(EventCallback callback);

    // 移除全局事件监听器
    bool removeGlobalEventListener(const GlobalEventListenerHandle &handle);

    // 事件触发 - 动态选择分发方式
    void triggerEvent(EventType eventType,
                      std::shared_ptr<EventArgs> args = nullptr,
                      ConnectionType connectType = ConnectionType::kAuto);

    // 异步事件处理
    bool processAsyncEvents();    // 主线程调用poll
    void startAsyncProcessing();  // 启动后台线程
    void stopAsyncProcessing();   // 停止后台线程

    // 状态查询
    bool isAsyncProcessingActive() const;
    // 获得所有事件类型
    static std::vector<EventType> allEventTypes();
    // 获得事件名称
    static std::string getEventTypeName(EventType type);

    // 获取底层分发器（用于扩展）
    SyncEventDispatcher *getSyncDispatcher();
    AsyncEventQueue *getAsyncQueue();

private:
    // 内部方法
    void asyncProcessingLoop();
    ConnectionType determineConnectionType(ConnectionType requested) const;
    bool isMainThread() const;

    void triggerEventSync(EventType eventType,
                          std::shared_ptr<EventArgs> args = nullptr);
    void triggerEventAsync(EventType eventType,
                           std::shared_ptr<EventArgs> args = nullptr);

private:
    // 统一的监听器存储 - 所有监听器都注册到这里
    std::unique_ptr<SyncEventDispatcher> unifiedDispatcher_;

    // 异步队列用于跨线程事件传递
    std::unique_ptr<AsyncEventQueue> asyncQueue_;

    // 异步处理
    std::atomic<bool> asyncProcessingActive_{false};
    std::atomic<bool> stopAsyncProcessing_{false};
    std::thread asyncProcessingThread_;

    // 记录主线程ID
    std::thread::id mainThreadId_;
};