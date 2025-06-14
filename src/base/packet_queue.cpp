#include "packet_queue.h"

#include <algorithm>
#include <stdexcept>

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

PacketQueue::PacketQueue(int maxPacketCount)
    : maxPacketCount_(std::max(1, maxPacketCount))
{
}

PacketQueue::~PacketQueue()
{
    abort();

    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
}

bool PacketQueue::push(const Packet &pkt, int timeoutMs)
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (timeoutMs < 0) {
        // 无限等待
        pushCond_.wait(lock, [this] { return canPush(); });
    } else if (timeoutMs == 0) {
        // 立即返回
        if (!canPush()) {
            return false;
        }
    } else {
        // 超时等待
        if (!pushCond_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                [this] { return canPush(); })) {
            return false;
        }
    }

    // 检查是否已中止
    if (aborted_.load(std::memory_order_acquire)) {
        return false;
    }

    // 添加到队列
    queue_.push_back(pkt);
    updateStatisticsOnPush(pkt);

    // 通知等待pop的线程
    popCond_.notify_one();
    return true;
}

bool PacketQueue::pop(Packet &pkt, int timeoutMs)
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (timeoutMs < 0) {
        // 无限等待
        popCond_.wait(lock, [this] { return canPop(); });
    } else if (timeoutMs == 0) {
        // 立即返回
        if (!canPop()) {
            return false;
        }
    } else {
        // 超时等待
        if (!popCond_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                               [this] { return canPop(); })) {
            return false;
        }
    }

    // 如果队列为空且已中止，返回false
    if (queue_.empty() && aborted_.load(std::memory_order_acquire)) {
        return false;
    }

    // 从队列中取出数据包
    pkt = std::move(queue_.front());
    queue_.pop_front();
    updateStatisticsOnPop(pkt);

    // 通知等待push的线程
    pushCond_.notify_one();
    return true;
}

bool PacketQueue::tryPop(Packet &pkt)
{
    return pop(pkt, 0);
}

bool PacketQueue::front(Packet &pkt) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (queue_.empty()) {
        return false;
    }

    pkt = queue_.front();
    return true;
}

void PacketQueue::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);

    queue_.clear();
    size_.store(0, std::memory_order_release);
    duration_.store(0, std::memory_order_release);
    serial_.fetch_add(1, std::memory_order_acq_rel);

    // 唤醒所有等待的线程
    pushCond_.notify_all();
    popCond_.notify_all();
}

void PacketQueue::start()
{
    std::lock_guard<std::mutex> lock(mutex_);

    aborted_.store(false, std::memory_order_release);
    serial_.fetch_add(1, std::memory_order_acq_rel);

    pushCond_.notify_all();
    popCond_.notify_all();
}

void PacketQueue::abort()
{
    std::lock_guard<std::mutex> lock(mutex_);

    aborted_.store(true, std::memory_order_release);

    // 唤醒所有等待的线程
    pushCond_.notify_all();
    popCond_.notify_all();
}

bool PacketQueue::isAborted() const noexcept
{
    return aborted_.load(std::memory_order_acquire);
}

size_t PacketQueue::packetCount() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

size_t PacketQueue::packetSize() const noexcept
{
    return size_.load(std::memory_order_acquire);
}

int64_t PacketQueue::packetDuration() const noexcept
{
    return duration_.load(std::memory_order_acquire);
}

size_t PacketQueue::maxPacketCount() const noexcept
{
    return maxPacketCount_.load(std::memory_order_acquire);
}

int PacketQueue::serial() const noexcept
{
    return serial_.load(std::memory_order_acquire);
}

bool PacketQueue::isFull() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() >= maxPacketCount_.load(std::memory_order_acquire);
}

bool PacketQueue::isEmpty() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

void PacketQueue::setMaxPacketCount(size_t maxCount)
{
    if (maxCount == 0) {
        throw std::invalid_argument("maxCount must be positive");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    maxPacketCount_.store(maxCount, std::memory_order_release);
    pushCond_.notify_all();
}

PacketQueue::Statistics PacketQueue::getStatistics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return {queue_.size(), size_.load(std::memory_order_acquire),
            duration_.load(std::memory_order_acquire),
            serial_.load(std::memory_order_acquire),
            aborted_.load(std::memory_order_acquire)};
}

bool PacketQueue::canPush() const noexcept
{
    return aborted_.load(std::memory_order_acquire) ||
           queue_.size() < maxPacketCount_.load(std::memory_order_acquire);
}

bool PacketQueue::canPop() const noexcept
{
    return aborted_.load(std::memory_order_acquire) || !queue_.empty();
}

void PacketQueue::updateStatisticsOnPush(const Packet &pkt) noexcept
{
    if (pkt.isValid()) {
        size_.fetch_add(pkt.size(), std::memory_order_acq_rel);
        duration_.fetch_add(pkt.duration(), std::memory_order_acq_rel);
    }
}

void PacketQueue::updateStatisticsOnPop(const Packet &pkt) noexcept
{
    if (pkt.isValid()) {
        size_.fetch_sub(pkt.size(), std::memory_order_acq_rel);
        duration_.fetch_sub(pkt.duration(), std::memory_order_acq_rel);
    }
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END