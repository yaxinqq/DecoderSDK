#include "event_dispatcher.h"
#include "logger/logger.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

EventDispatcher::EventDispatcher()
    : unifiedDispatcher_(std::make_unique<SyncEventDispatcher>()),
      asyncQueue_(std::make_unique<AsyncEventQueue>()),
      mainThreadId_(std::this_thread::get_id())
{
    // 为异步队列注册统一的监听器，用来转发事件类型
    for (const auto &eventType : allEventTypes()) {
        asyncQueue_->appendListener(eventType,
                                    [this](EventType eventType, std::shared_ptr<EventArgs> args) {
                                        unifiedDispatcher_->dispatch(eventType, args);
                                    });
    }
}

EventDispatcher::~EventDispatcher()
{
    stopAsyncProcessing();
}

EventListenerHandle EventDispatcher::addEventListener(EventType eventType,
                                                      const std::function<EventCallback> &callback)
{
    // 统一注册到unifiedDispatcher_，不再区分连接类型
    const auto realHandle = unifiedDispatcher_->appendListener(eventType, callback);
    const auto handle = incredNum_++;
    // 保存到map中
    handleMap_.insert(std::make_pair(handle, realHandle));

    return handle;
}

bool EventDispatcher::removeEventListener(EventType eventType, EventListenerHandle handle)
{
    if (handleMap_.find(handle) == handleMap_.end()) {
        return false;
    }

    // 得到真实句柄
    const auto realHandle = handleMap_.at(handle);

    // 从map中移除
    handleMap_.erase(handle);
    return unifiedDispatcher_->removeListener(eventType, realHandle);
}

GlobalEventListenerHandle EventDispatcher::addGlobalEventListener(
    const std::function<EventCallback> &callback)
{
    GlobalEventListenerHandle handle;
    for (const auto &eventType : allEventTypes()) {
        handle.insert(std::make_pair<>(eventType, addEventListener(eventType, callback)));
    }

    return handle;
}

bool EventDispatcher::removeGlobalEventListener(const GlobalEventListenerHandle &handle)
{
    if (handle.empty()) {
        return false;
    }

    for (const auto &pair : handle) {
        removeEventListener(pair.first, pair.second);
    }

    return true;
}

void EventDispatcher::triggerEvent(EventType eventType, std::shared_ptr<EventArgs> args,
                                   ConnectionType connectType)
{
    // 动态选择分发方式
    ConnectionType actualType = determineConnectionType(connectType);
    switch (actualType) {
        case ConnectionType::kAuto:
        case ConnectionType::kDirect:
            triggerEventSync(eventType, args);
            break;
        case ConnectionType::kQueued:
            triggerEventAsync(eventType, args);
            break;
        default:
            break;
    }
}

bool EventDispatcher::processAsyncEvents()
{
    // 处理异步队列中的事件
    return asyncQueue_->process();
}

void EventDispatcher::startAsyncProcessing()
{
    if (asyncProcessingActive_.load()) {
        return; // 已经在运行
    }

    stopAsyncProcessing_.store(false);
    asyncProcessingActive_.store(true);

    asyncProcessingThread_ = std::thread([this]() { asyncProcessingLoop(); });
}

void EventDispatcher::stopAsyncProcessing()
{
    if (!asyncProcessingActive_.load()) {
        return; // 没有在运行
    }

    stopAsyncProcessing_.store(true);
    asyncProcessingActive_.store(false);

    if (asyncProcessingThread_.joinable()) {
        asyncProcessingThread_.join();
    }
}

bool EventDispatcher::isAsyncProcessingActive() const
{
    return asyncProcessingActive_.load();
}

SyncEventDispatcher *EventDispatcher::getSyncDispatcher()
{
    return unifiedDispatcher_.get();
}

AsyncEventQueue *EventDispatcher::getAsyncQueue()
{
    return asyncQueue_.get();
}

void EventDispatcher::asyncProcessingLoop()
{
    while (!stopAsyncProcessing_.load()) {
        try {
            // 等待异步队列中的事件
            if (asyncQueue_->waitFor(std::chrono::milliseconds(10))) {
                // 处理异步事件队列
                asyncQueue_->process();
            }
        } catch (const std::exception &e) {
            LOG_ERROR("Exception in async processing loop: {}", e.what());
        }
    }
}

ConnectionType EventDispatcher::determineConnectionType(ConnectionType requested) const
{
    if (requested == ConnectionType::kAuto) {
        return isMainThread() ? ConnectionType::kDirect : ConnectionType::kQueued;
    }

    return requested;
}

bool EventDispatcher::isMainThread() const
{
    return std::this_thread::get_id() == mainThreadId_;
}

void EventDispatcher::triggerEventSync(EventType eventType, std::shared_ptr<EventArgs> args)
{
    try {
        unifiedDispatcher_->dispatch(eventType, args);
    } catch (const std::exception &e) {
        LOG_ERROR("Exception in sync event dispatch: {}", e.what());
    }
}

void EventDispatcher::triggerEventAsync(EventType eventType, std::shared_ptr<EventArgs> args)
{
    // 将事件加入异步队列
    asyncQueue_->enqueue(eventType, args);
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END