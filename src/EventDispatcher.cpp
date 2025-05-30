#include "EventDispatcher.h"
#include "Logger.h"
#include <algorithm>
#include <queue>
#include <shared_mutex>

EventDispatcher::EventDispatcher()
    : handleGenerator_(1),
      asyncProcessingEnabled_(true),
      stopEventProcessing_(false),
      maxEventQueueSize_(1000),
      totalEventsTriggered_(0),
      totalEventsProcessed_(0)
{
    // 启动事件处理线程
    eventProcessingThread_ =
        std::thread(&EventDispatcher::eventProcessingLoop, this);

    LOG_DEBUG("EventDispatcher initialized");
}

EventDispatcher::~EventDispatcher()
{
    // 停止事件处理线程
    stopEventProcessing_ = true;
    eventQueueCv_.notify_all();

    if (eventProcessingThread_.joinable()) {
        eventProcessingThread_.join();
    }

    // 清理所有监听器
    removeAllListeners();

    LOG_DEBUG(
        "EventDispatcher destroyed. Total events triggered: {}, processed: {}",
        totalEventsTriggered_.load(), totalEventsProcessed_.load());
}

EventListenerHandle EventDispatcher::addEventListener(EventType eventType,
                                                      EventCallback callback)
{
    if (!callback) {
        LOG_WARN("Attempting to add null callback for event type {}",
                 static_cast<int>(eventType));
        return 0;
    }

    std::unique_lock<std::shared_mutex> lock(listenersMutex_);

    EventListenerHandle handle = generateHandle();
    listeners_[eventType].emplace_back(
        ListenerInfo{handle, std::move(callback)});

    LOG_DEBUG("Added event listener for type {} with handle {}",
              static_cast<int>(eventType), handle);

    return handle;
}

bool EventDispatcher::removeEventListener(EventType eventType,
                                          EventListenerHandle handle)
{
    std::unique_lock<std::shared_mutex> lock(listenersMutex_);

    auto it = listeners_.find(eventType);
    if (it == listeners_.end()) {
        return false;
    }

    auto &listenerList = it->second;
    auto listenerIt = std::find_if(
        listenerList.begin(), listenerList.end(),
        [handle](const ListenerInfo &info) { return info.handle == handle; });

    if (listenerIt != listenerList.end()) {
        listenerList.erase(listenerIt);

        // 如果该事件类型没有监听器了，移除整个条目
        if (listenerList.empty()) {
            listeners_.erase(it);
        }

        LOG_DEBUG("Removed event listener for type {} with handle {}",
                  static_cast<int>(eventType), handle);
        return true;
    }

    return false;
}

void EventDispatcher::removeAllListeners(EventType eventType)
{
    std::unique_lock<std::shared_mutex> lock(listenersMutex_);

    auto it = listeners_.find(eventType);
    if (it != listeners_.end()) {
        size_t count = it->second.size();
        listeners_.erase(it);
        LOG_DEBUG("Removed {} listeners for event type {}", count,
                  static_cast<int>(eventType));
    }
}

void EventDispatcher::triggerEvent(EventType eventType,
                                   std::shared_ptr<EventArgs> args)
{
    totalEventsTriggered_++;
    triggerEventInternal(eventType, args);
}

void EventDispatcher::triggerEventAsync(EventType eventType,
                                        std::shared_ptr<EventArgs> args)
{
    if (!asyncProcessingEnabled_) {
        triggerEvent(eventType, args);
        return;
    }

    totalEventsTriggered_++;

    {
        std::lock_guard<std::mutex> lock(eventQueueMutex_);

        // 检查队列大小限制
        if (eventQueue_.size() >= maxEventQueueSize_) {
            LOG_WARN("Event queue is full, dropping oldest event");
            eventQueue_.pop();
        }

        eventQueue_.emplace(AsyncEvent{eventType, args});
    }

    eventQueueCv_.notify_one();
}

void EventDispatcher::setAsyncProcessing(bool enabled)
{
    asyncProcessingEnabled_ = enabled;
    LOG_DEBUG("Async event processing {}", enabled ? "enabled" : "disabled");
}

size_t EventDispatcher::getListenerCount(EventType eventType) const
{
    std::shared_lock<std::shared_mutex> lock(listenersMutex_);

    auto it = listeners_.find(eventType);
    return (it != listeners_.end()) ? it->second.size() : 0;
}

bool EventDispatcher::hasListeners(EventType eventType) const
{
    return getListenerCount(eventType) > 0;
}

void EventDispatcher::setMaxEventQueueSize(size_t maxSize)
{
    maxEventQueueSize_ = maxSize;
    LOG_DEBUG("Set max event queue size to {}", maxSize);
}

size_t EventDispatcher::getPendingEventCount() const
{
    std::lock_guard<std::mutex> lock(eventQueueMutex_);
    return eventQueue_.size();
}

void EventDispatcher::clearPendingEvents()
{
    std::lock_guard<std::mutex> lock(eventQueueMutex_);

    size_t count = eventQueue_.size();
    std::queue<AsyncEvent> empty;
    eventQueue_.swap(empty);

    LOG_DEBUG("Cleared {} pending events", count);
}

void EventDispatcher::waitForPendingEvents()
{
    while (getPendingEventCount() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void EventDispatcher::eventProcessingLoop()
{
    LOG_DEBUG("Event processing thread started");

    while (!stopEventProcessing_) {
        AsyncEvent event;

        {
            std::unique_lock<std::mutex> lock(eventQueueMutex_);
            eventQueueCv_.wait(lock, [this] {
                return !eventQueue_.empty() || stopEventProcessing_;
            });

            if (stopEventProcessing_) {
                break;
            }

            if (!eventQueue_.empty()) {
                event = eventQueue_.front();
                eventQueue_.pop();
            } else {
                continue;
            }
        }

        try {
            triggerEventInternal(event.eventType, event.args);
            totalEventsProcessed_++;
        } catch (const std::exception &e) {
            LOG_ERROR("Exception in event processing: {}", e.what());
        } catch (...) {
            LOG_ERROR("Unknown exception in event processing");
        }
    }

    LOG_DEBUG("Event processing thread stopped");
}

std::string EventDispatcher::getEventTypeName(EventType eventType)
{
    switch (eventType) {
        // 流事件
        case EventType::kStreamOpened:
            return "StreamOpened";
        case EventType::kStreamClosed:
            return "StreamClosed";
        case EventType::kStreamOpening:
            return "StreamOpening";
        case EventType::kStreamOpenFailed:
            return "StreamOpenFailed";
        case EventType::kStreamClose:
            return "StreamClose";
        case EventType::kStreamReadData:
            return "StreamReadData";
        case EventType::kStreamReadError:
            return "StreamReadError";
        case EventType::kStreamReadRecovery:
            return "StreamReadRecovery";
        case EventType::kStreamEnded:
            return "StreamEnded";

        // 解码相关事件
        case EventType::kDecodeStarted:
            return "DecodeStarted";
        case EventType::kDecodeStopped:
            return "DecodeStopped";
        case EventType::kDecodePaused:
            return "DecodePaused";
        case EventType::kCreateDecoderSuccess:
            return "CreateDecoderSuccess";
        case EventType::kCreateDecoderFailed:
            return "CreateDecoderFailed";
        case EventType::kDestoryDecoder:
            return "DestoryDecoder";
        case EventType::kDecodeFirstFrame:
            return "DecodeFirstFrame";
        case EventType::kDecodeError:
            return "DecodeError";
        case EventType::kDecodeRecovery:
            return "DecodeRecovery";

        // seek相关事件
        case EventType::kSeekStarted:
            return "SeekStarted";
        case EventType::kSeekSuccess:
            return "SeekSuccess";
        case EventType::kSeekFailed:
            return "SeekFailed";

        // 录制相关事件
        case EventType::kRecordingStarted:
            return "RecordingStarted";
        case EventType::kRecordingStopped:
            return "RecordingStopped";
        case EventType::kRecordingError:
            return "RecordingError";

        default:
            return "Unknown";
    }
}

void EventDispatcher::setEventLogging(bool enabled)
{
    eventLoggingEnabled_ = enabled;
    LOG_DEBUG("Event logging {}", enabled ? "enabled" : "disabled");
}

EventDispatcher::EventStats EventDispatcher::getEventStats() const
{
    EventStats stats;
    stats.totalTriggered = totalEventsTriggered_.load();
    stats.totalProcessed = totalEventsProcessed_.load();
    stats.pendingCount = getPendingEventCount();

    std::lock_guard<std::mutex> lock(eventCountsMutex_);
    stats.eventCounts = eventCounts_;

    return stats;
}

void EventDispatcher::logEvent(EventType eventType,
                               std::shared_ptr<EventArgs> args)
{
    if (!eventLoggingEnabled_.load()) {
        return;
    }

    std::string eventName = getEventTypeName(eventType);
    std::string source = args ? args->source : "Unknown";

    LOG_DEBUG("Event triggered: {} from {}", eventName, source);

    // 更新事件计数
    {
        std::lock_guard<std::mutex> lock(eventCountsMutex_);
        eventCounts_[eventType]++;
    }
}

EventListenerHandle EventDispatcher::generateHandle()
{
    return handleGenerator_.fetch_add(1);
}

// 添加全局事件监听器（监听所有事件类型）
EventListenerHandle EventDispatcher::addGlobalEventListener(
    EventCallback callback)
{
    if (!callback) {
        LOG_WARN("Attempting to add null global callback");
        return 0;
    }

    std::unique_lock<std::shared_mutex> lock(globalListenersMutex_);

    EventListenerHandle handle = generateHandle();
    globalListeners_.emplace_back(ListenerInfo{handle, std::move(callback)});

    LOG_DEBUG("Added global event listener with handle {}", handle);

    return handle;
}

// 移除全局事件监听器
bool EventDispatcher::removeGlobalEventListener(EventListenerHandle handle)
{
    std::unique_lock<std::shared_mutex> lock(globalListenersMutex_);

    auto it = std::find_if(
        globalListeners_.begin(), globalListeners_.end(),
        [handle](const ListenerInfo &info) { return info.handle == handle; });

    if (it != globalListeners_.end()) {
        globalListeners_.erase(it);
        LOG_DEBUG("Removed global event listener with handle {}", handle);
        return true;
    }

    return false;
}

// 移除所有全局事件监听器
void EventDispatcher::removeAllGlobalListeners()
{
    std::unique_lock<std::shared_mutex> lock(globalListenersMutex_);

    size_t count = globalListeners_.size();
    globalListeners_.clear();

    LOG_DEBUG("Removed {} global event listeners", count);
}

// 获取全局监听器数量
size_t EventDispatcher::getGlobalListenerCount() const
{
    std::shared_lock<std::shared_mutex> lock(globalListenersMutex_);
    return globalListeners_.size();
}

// 修改triggerEventInternal方法，添加全局监听器调用
void EventDispatcher::triggerEventInternal(EventType eventType,
                                           std::shared_ptr<EventArgs> args)
{
    // 设置事件源（如果args存在且source为空）
    if (args && args->source.empty()) {
        args->source = "EventDispatcher";
    }

    // 记录事件日志
    logEvent(eventType, args);

    // 首先调用全局监听器
    {
        std::shared_lock<std::shared_mutex> globalLock(globalListenersMutex_);
        auto globalListenersCopy = globalListeners_;
        globalLock.unlock();

        for (const auto &listenerInfo : globalListenersCopy) {
            try {
                listenerInfo.callback(eventType, args);
            } catch (const std::exception &e) {
                LOG_ERROR("Exception in global event callback for type {}: {}",
                          static_cast<int>(eventType), e.what());
            } catch (...) {
                LOG_ERROR(
                    "Unknown exception in global event callback for type {}",
                    static_cast<int>(eventType));
            }
        }
    }

    // 然后调用特定事件类型的监听器
    std::shared_lock<std::shared_mutex> lock(listenersMutex_);

    auto it = listeners_.find(eventType);
    if (it == listeners_.end()) {
        return;  // 没有特定类型的监听器
    }

    // 复制监听器列表以避免在回调中修改监听器时出现问题
    auto listenersCopy = it->second;
    lock.unlock();

    // 调用特定事件类型的监听器
    for (const auto &listenerInfo : listenersCopy) {
        try {
            listenerInfo.callback(eventType, args);
        } catch (const std::exception &e) {
            LOG_ERROR("Exception in event callback for type {}: {}",
                      static_cast<int>(eventType), e.what());
        } catch (...) {
            LOG_ERROR("Unknown exception in event callback for type {}",
                      static_cast<int>(eventType));
        }
    }
}

// 修改removeAllListeners方法，同时清理全局监听器
void EventDispatcher::removeAllListeners()
{
    {
        std::unique_lock<std::shared_mutex> lock(listenersMutex_);

        size_t totalCount = 0;
        for (const auto &pair : listeners_) {
            totalCount += pair.second.size();
        }

        listeners_.clear();
        LOG_DEBUG("Removed all {} specific event listeners", totalCount);
    }

    // 同时清理全局监听器
    removeAllGlobalListeners();
}
