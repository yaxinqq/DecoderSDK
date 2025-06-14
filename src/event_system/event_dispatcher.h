#ifndef DECODER_SDK_INTERNAL_EVENT_DISPATCHER_H
#define DECODER_SDK_INTERNAL_EVENT_DISPATCHER_H
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

#include <eventpp/eventdispatcher.h>
#include <eventpp/eventqueue.h>

#include "base/base_define.h"
#include "include/decodersdk/common_define.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

// 基于eventpp的事件分发器类型定义
// 事件策略配置
struct EventPolicy {
    using Threading = eventpp::MultipleThreading;
};

using SyncEventDispatcher = eventpp::EventDispatcher<EventType, EventCallback, EventPolicy>;
using AsyncEventQueue = eventpp::EventQueue<EventType, EventCallback, EventPolicy>;

/**
 * 统一的事件分发器
 * 支持同步和异步事件分发，基于eventpp库
 * 监听器统一注册，事件触发时动态选择分发方式
 */
class EventDispatcher {
public:
    /**
     * @brief 构造函数
     */
    EventDispatcher();
    /**
     * @brief 析构函数
     */
    ~EventDispatcher();

    // 禁用拷贝和移动
    EventDispatcher(const EventDispatcher &) = delete;
    EventDispatcher &operator=(const EventDispatcher &) = delete;
    EventDispatcher(EventDispatcher &&) = delete;
    EventDispatcher &operator=(EventDispatcher &&) = delete;

    /**
     * @brief 添加事件监听器
     * @param eventType 事件类型
     * @param callback 回调函数
     * @return 监听器句柄
     */
    uint64_t addEventListener(EventType eventType, const std::function<EventCallback> &callback);

    /**
     * @brief 移除事件监听器
     * @param eventType 事件类型
     * @param handle 监听器句柄
     * @return true 移除成功; false 移除失败
     */
    bool removeEventListener(EventType eventType, uint64_t handle);

    /**
     * @brief 添加全局事件监听器
     * @param eventType 事件类型
     * @param callback 回调函数
     * @return GlobalEventListenerHandle 监听器句柄
     */
    GlobalEventListenerHandle addGlobalEventListener(EventCallback callback);

    /**
     * @brief 移除全局事件监听器
     * @param eventType 事件类型
     * @param handle 监听器句柄
     * @return true 移除成功; false 移除失败
     */
    bool removeGlobalEventListener(const GlobalEventListenerHandle &handle);

    /**
     * @brief 触发事件
     * @param eventType 事件类型
     * @param args 事件参数
     * @param connectType 连接类型
     */
    void triggerEvent(EventType eventType, std::shared_ptr<EventArgs> args = nullptr,
                      ConnectionType connectType = ConnectionType::kAuto);

    /**
     * @brief 处理异步事件, 主线程调用poll
     * @return true 有事件处理; false 无事件
     */
    bool processAsyncEvents();
    /**
     * @brief 启动异步处理线程
     */
    void startAsyncProcessing();
    /**
     * @brief 停止异步处理线程
     */
    void stopAsyncProcessing();

    /**
     * @brief 获取异步处理状态
     * @return true 异步处理线程运行中; false 异步处理线程已停止
     */
    bool isAsyncProcessingActive() const;

    // 用于扩展
    /**
     * @brief 获取同步事件分发器
     * @return SyncEventDispatcher* 同步事件分发器指针
     */
    SyncEventDispatcher *getSyncDispatcher();
    /**
     * @brief 获取异步事件队列
     * @return AsyncEventQueue* 异步事件队列指针
     */
    AsyncEventQueue *getAsyncQueue();

private:
    /**
     * @brief 异步处理线程主循环
     */
    void asyncProcessingLoop();
    /**
     * @brief 确定连接类型
     * @param requested 请求的连接类型
     * @return ConnectionType 实际的连接类型
     */
    ConnectionType determineConnectionType(ConnectionType requested) const;
    /**
     * @brief 判断是否为主线程
     * @return true 当前线程为主线程; false 当前线程为非主线程
     */
    bool isMainThread() const;

    /**
     * @brief 触发同步事件
     * @param eventType 事件类型
     * @param args 事件参数
     */
    void triggerEventSync(EventType eventType, std::shared_ptr<EventArgs> args = nullptr);
    /**
     * @brief 触发异步事件
     * @param eventType 事件类型
     * @param args 事件参数
     */
    void triggerEventAsync(EventType eventType, std::shared_ptr<EventArgs> args = nullptr);

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

    // 自增数字和Handle的映射表
    std::unordered_map<EventListenerHandle, SyncEventDispatcher::Handle> handleMap_;
    EventListenerHandle incredNum_ = 0;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_EVENT_DISPATCHER_H