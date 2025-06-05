#include <algorithm>
#include <iostream>

#include "magic_enum/magic_enum.hpp"

#include "EventDispatcher.h"

EventDispatcher::EventDispatcher()
    : syncDispatcher_(std::make_unique<SyncEventDispatcher>()),
      asyncQueue_(std::make_unique<AsyncEventQueue>()),
      mainThreadId_(std::this_thread::get_id())
{
}

EventDispatcher::~EventDispatcher()
{
    stopAsyncProcessing();
}

EventListenerHandle EventDispatcher::addEventListener(
    EventType eventType, const std::function<EventCallback> &callback,
    ConnectionType connectionType)
{
    ConnectionType actualType = determineConnectionType(connectionType);

    if (actualType == ConnectionType::kDirect) {
        // 同步监听器
        return syncDispatcher_->appendListener(eventType, callback);
    } else {
        // 异步监听器 - 注册到异步队列
        return asyncQueue_->appendListener(eventType, callback);
    }
}

bool EventDispatcher::removeEventListener(EventType eventType,
                                          EventListenerHandle handle)
{
    return syncDispatcher_->removeListener(eventType, handle);
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
                                   std::shared_ptr<EventArgs> args)
{
    ConnectionType type = determineConnectionType(ConnectionType::kAuto);
    if (type == ConnectionType::kAuto) {
        // 自动选择：默认使用同步
        triggerEventSync(eventType, args);
    } else if (type == ConnectionType::kDirect) {
        triggerEventSync(eventType, args);
    } else {
        triggerEventAsync(eventType, args);
    }
}

void EventDispatcher::triggerEventSync(EventType eventType,
                                       std::shared_ptr<EventArgs> args)
{
    try {
        syncDispatcher_->dispatch(eventType, args);
    } catch (const std::exception &e) {
        std::cerr << "Exception in sync event dispatch: " << e.what()
                  << std::endl;
    }
}

void EventDispatcher::triggerEventAsync(EventType eventType,
                                        std::shared_ptr<EventArgs> args)
{
    asyncQueue_->enqueue(eventType, args);
}

void EventDispatcher::processAsyncEvents()
{
    // 处理异步队列中的事件
    asyncQueue_->process();
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
    return syncDispatcher_.get();
}

AsyncEventQueue *EventDispatcher::getAsyncQueue()
{
    return asyncQueue_.get();
}

void EventDispatcher::asyncProcessingLoop()
{
    while (!stopAsyncProcessing_.load()) {
        try {
            // 处理异步事件队列
            processAsyncEvents();

            // 短暂休眠以避免过度占用CPU
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
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