#include "EventDispatcher.h"
#include <iostream>

#include "magic_enum/magic_enum.hpp"

EventDispatcher::EventDispatcher()
    : unifiedDispatcher_(std::make_unique<SyncEventDispatcher>()),
      asyncQueue_(std::make_unique<AsyncEventQueue>()),
      mainThreadId_(std::this_thread::get_id())
{
    // 为异步队列注册统一的监听器，用来转发事件类型
    for (const auto &eventType : allEventTypes()) {
        asyncQueue_->appendListener(
            eventType,
            [this](EventType eventType, std::shared_ptr<EventArgs> args) {
                unifiedDispatcher_->dispatch(eventType, args);
            });
    }
}

EventDispatcher::~EventDispatcher()
{
    stopAsyncProcessing();
}

EventListenerHandle EventDispatcher::addEventListener(
    EventType eventType, const std::function<EventCallback> &callback)
{
    // 统一注册到unifiedDispatcher_，不再区分连接类型
    return unifiedDispatcher_->appendListener(eventType, callback);
}

bool EventDispatcher::removeEventListener(EventType eventType,
                                          EventListenerHandle handle)
{
    return unifiedDispatcher_->removeListener(eventType, handle);
}

GlobalEventListenerHandle EventDispatcher::addGlobalEventListener(
    EventCallback callback)
{
    GlobalEventListenerHandle handle;
    for (const auto &eventType : allEventTypes()) {
        handle.insert(
            std::make_pair<>(eventType, addEventListener(eventType, callback)));
    }

    return handle;
}

bool EventDispatcher::removeGlobalEventListener(
    const GlobalEventListenerHandle &handle)
{
    if (handle.empty()) {
        return false;
    }

    for (const auto &pair : handle) {
        removeEventListener(pair.first, pair.second);
    }

    return true;
}

void EventDispatcher::triggerEvent(EventType eventType,
                                   std::shared_ptr<EventArgs> args,
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

void EventDispatcher::triggerEventSync(EventType eventType,
                                       std::shared_ptr<EventArgs> args)
{
    try {
        unifiedDispatcher_->dispatch(eventType, args);
    } catch (const std::exception &e) {
        std::cerr << "Exception in sync event dispatch: " << e.what()
                  << std::endl;
    }
}

void EventDispatcher::triggerEventAsync(EventType eventType,
                                        std::shared_ptr<EventArgs> args)
{
    // 将事件加入异步队列
    asyncQueue_->enqueue(eventType, args);
}

bool EventDispatcher::processAsyncEvents()
{
    // 处理异步队列中的事件
    return asyncQueue_->process();
}

void EventDispatcher::startAsyncProcessing()
{
    if (asyncProcessingActive_.load()) {
        return;  // 已经在运行
    }

    stopAsyncProcessing_.store(false);
    asyncProcessingActive_.store(true);

    asyncProcessingThread_ = std::thread([this]() { asyncProcessingLoop(); });
}

void EventDispatcher::stopAsyncProcessing()
{
    if (!asyncProcessingActive_.load()) {
        return;  // 没有在运行
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

std::vector<EventType> EventDispatcher::allEventTypes()
{
    std::vector<EventType> types;
    for (const auto &type : magic_enum::enum_values<EventType>()) {
        types.emplace_back(type);
    }

    return types;
}

std::string EventDispatcher::getEventTypeName(EventType type)
{
    if (!magic_enum::enum_contains(type)) {
        return {};
    }

    return std::string(magic_enum::enum_name(type));
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
            std::cerr << "Exception in async processing loop: " << e.what()
                      << std::endl;
        }
    }
}

ConnectionType EventDispatcher::determineConnectionType(
    ConnectionType requested) const
{
    if (requested == ConnectionType::kAuto) {
        return isMainThread() ? ConnectionType::kDirect
                              : ConnectionType::kQueued;
    }

    return requested;
}

bool EventDispatcher::isMainThread() const
{
    return std::this_thread::get_id() == mainThreadId_;
}