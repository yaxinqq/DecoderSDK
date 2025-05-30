#pragma once
#include <any>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
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

// 事件回调函数类型
using EventCallback =
    std::function<void(EventType, std::shared_ptr<EventArgs>)>;

// 事件监听器句柄
using EventListenerHandle = uint64_t;

// 事件分发器类
class EventDispatcher {
public:
    EventDispatcher();
    ~EventDispatcher();

    // 禁用拷贝构造和赋值
    EventDispatcher(const EventDispatcher &) = delete;
    EventDispatcher &operator=(const EventDispatcher &) = delete;

    // 设置全部事件的监听器
    EventListenerHandle addGlobalEventListener(EventCallback callback);

    // 移除全局事件监听器
    bool removeGlobalEventListener(EventListenerHandle handle);

    // 移除所有全局事件监听器
    void removeAllGlobalListeners();

    // 获取全局监听器数量
    size_t getGlobalListenerCount() const;

    // 添加事件监听器
    EventListenerHandle addEventListener(EventType eventType,
                                         EventCallback callback);

    // 移除事件监听器
    bool removeEventListener(EventType eventType, EventListenerHandle handle);

    // 移除指定事件类型的所有监听器
    void removeAllListeners(EventType eventType);

    // 移除所有监听器
    void removeAllListeners();

    // 同步触发事件（在当前线程中立即执行）
    void triggerEvent(EventType eventType,
                      std::shared_ptr<EventArgs> args = nullptr);

    // 异步触发事件（在事件处理线程中执行）
    void triggerEventAsync(EventType eventType,
                           std::shared_ptr<EventArgs> args = nullptr);

    // 启用/禁用异步事件处理
    void setAsyncProcessing(bool enabled);

    // 获取监听器数量
    size_t getListenerCount(EventType eventType) const;

    // 检查是否有监听器
    bool hasListeners(EventType eventType) const;

    // 设置事件队列最大大小（用于异步处理）
    void setMaxEventQueueSize(size_t maxSize);

    // 获取待处理事件数量
    size_t getPendingEventCount() const;

    // 清空待处理事件队列
    void clearPendingEvents();

    // 等待所有待处理事件完成
    void waitForPendingEvents();

    // 获取事件类型名称（用于调试）
    static std::string getEventTypeName(EventType eventType);

    // 启用/禁用事件日志
    void setEventLogging(bool enabled);

    // 获取事件统计信息
    struct EventStats {
        size_t totalTriggered = 0;
        size_t totalProcessed = 0;
        size_t pendingCount = 0;
        std::unordered_map<EventType, size_t> eventCounts;
    };
    EventStats getEventStats() const;

private:
    // 事件处理线程函数
    void eventProcessingLoop();

    // 内部触发事件实现
    void triggerEventInternal(EventType eventType,
                              std::shared_ptr<EventArgs> args);

    // 生成唯一的监听器句柄
    EventListenerHandle generateHandle();

    // 记录事件日志
    void logEvent(EventType eventType, std::shared_ptr<EventArgs> args);

private:
    // 监听器存储结构
    struct ListenerInfo {
        EventListenerHandle handle;
        EventCallback callback;
    };

    // 异步事件结构
    struct AsyncEvent {
        EventType eventType;
        std::shared_ptr<EventArgs> args;
    };

    // 事件监听器映射表
    std::unordered_map<EventType, std::vector<ListenerInfo>> listeners_;

    // 保护监听器映射表的互斥锁
    mutable std::shared_mutex listenersMutex_;

    // 句柄生成器
    std::atomic<EventListenerHandle> handleGenerator_;

    // 异步事件处理相关
    std::atomic<bool> asyncProcessingEnabled_;
    std::atomic<bool> stopEventProcessing_;
    std::thread eventProcessingThread_;

    // 异步事件队列
    std::queue<AsyncEvent> eventQueue_;
    mutable std::mutex eventQueueMutex_;
    std::condition_variable eventQueueCv_;

    // 事件队列最大大小
    std::atomic<size_t> maxEventQueueSize_;

    // 统计信息
    std::atomic<size_t> totalEventsTriggered_;
    std::atomic<size_t> totalEventsProcessed_;
    mutable std::mutex eventCountsMutex_;
    std::unordered_map<EventType, size_t> eventCounts_;

    // 事件日志开关
    std::atomic<bool> eventLoggingEnabled_;

    // 全局事件监听器（监听所有事件类型）
    std::vector<ListenerInfo> globalListeners_;
    mutable std::shared_mutex globalListenersMutex_;
};