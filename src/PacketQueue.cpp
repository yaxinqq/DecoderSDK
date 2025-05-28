#include "PacketQueue.h"

#pragma region Packet
Packet::Packet() : packet_(av_packet_alloc()), serial_(0)
{
}

Packet::Packet(AVPacket* pkt) : Packet()
{
    av_packet_ref(packet_, pkt);
}

Packet::~Packet()
{
    if (packet_) {
        av_packet_free(&packet_);
    }
}

Packet::Packet(const Packet& other)
    : packet_(av_packet_alloc()), serial_(other.serial_)
{
    av_packet_ref(packet_, other.packet_);
}

Packet& Packet::operator=(const Packet& other)
{
    if (this != &other) {
        if (!packet_) {
            packet_ = av_packet_alloc();
        } else {
            av_packet_unref(packet_);
        }
        av_packet_ref(packet_, other.packet_);
        serial_ = other.serial_;
    }
    return *this;
}

Packet::Packet(Packet&& other) noexcept
    : packet_(other.packet_), serial_(other.serial_)
{
    other.packet_ = nullptr;
}

Packet& Packet::operator=(Packet&& other) noexcept
{
    if (this != &other) {
        if (packet_) {
            av_packet_free(&packet_);
        }
        packet_ = other.packet_;
        serial_ = other.serial_;
        other.packet_ = nullptr;
    }
    return *this;
}

AVPacket* Packet::get() const
{
    return packet_;
}

void Packet::setSerial(int serial)
{
    serial_ = serial;
}

int Packet::serial() const
{
    return serial_;
}

#pragma endregion

#pragma region PacketQueue

PacketQueue::PacketQueue(int maxPacketCount)
    : maxPacketCount_{maxPacketCount}, serial_{0}
{
    requestAborted_.store(false);
}

PacketQueue::~PacketQueue()
{
    while (!queue_.empty()) {
        queue_.pop();
    }
}

bool PacketQueue::push(const Packet& pkt, int delayTimeMs)
{
    std::unique_lock<std::mutex> lock(mutex_);

    // 唤醒条件变量的条件（请求终止或队列有空位）
    auto canPush = [this]() {
        return requestAborted_.load() || queue_.size() < maxPacketCount_;
    };

    if (delayTimeMs < 0) {
        // 无限阻塞
        cond_.wait(lock, canPush);
    } else if (delayTimeMs == 0) {
        // 立即返回
        if (!canPush())
            return false;
    } else {
        // 阻塞时长
        if (!cond_.wait_for(lock, std::chrono::milliseconds(delayTimeMs),
                            canPush)) {
            return false;
        }
    }

    // 检查是否请求终止
    if (requestAborted_.load())
        return false;

    // 入队
    queue_.push(pkt);
    // 增加数据大小和时长
    size_ += pkt.get()->size;
    duration_ += pkt.get()->duration;

    // 唤醒条件变量
    cond_.notify_one();
    return true;
}

bool PacketQueue::pop(Packet& pkt, int delayTimeMs)
{
    std::unique_lock<std::mutex> lock(mutex_);

    // 唤醒条件变量的条件（请求终止或队列非空）
    auto canPop = [this]() {
        return requestAborted_.load() || !queue_.empty();
    };

    if (delayTimeMs < 0) {
        // 无限阻塞
        if (!canPop())
            return false;
    } else if (delayTimeMs == 0) {
        // 立即返回
        cond_.wait(lock, canPop);
    } else {
        // 阻塞时长
        if (!cond_.wait_for(lock, std::chrono::milliseconds(delayTimeMs),
                            canPop)) {
            return false;
        }
    }

    // 检查是否请求终止
    if (requestAborted_ && queue_.empty())
        return false;

    // 出队
    pkt = queue_.front();
    queue_.pop();
    // 减少数据大小和时长
    size_ -= pkt.get()->size;
    duration_ -= pkt.get()->duration;

    // 唤醒条件变量
    cond_.notify_one();
    return true;
}

void PacketQueue::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
        queue_.pop();
    }

    size_ = 0;
    duration_ = 0;
    serial_++;
}

void PacketQueue::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    requestAborted_.store(false);
    serial_++;
}

void PacketQueue::abort()
{
    std::lock_guard<std::mutex> lock(mutex_);
    requestAborted_.store(true);
    cond_.notify_all();
}

bool PacketQueue::isAbort() const
{
    return requestAborted_.load();
}

int PacketQueue::packetCount() const
{
    return queue_.size();
}

int PacketQueue::packetSize() const
{
    return size_;
}

int64_t PacketQueue::packetDuration() const
{
    return duration_;
}

int PacketQueue::maxPacketCount() const
{
    return maxPacketCount_;
}

int PacketQueue::serial() const
{
    return serial_;
}

void PacketQueue::destory()
{
    flush();
    clear();
}

void PacketQueue::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
        queue_.pop();
    }
}

bool PacketQueue::isFull() const
{
    return queue_.size() >= maxPacketCount_;
}

bool PacketQueue::isEmpty() const
{
    return queue_.empty();
}

void PacketQueue::setMaxPacketCount(int maxPacketCount)
{
    if (maxPacketCount_ <= 0)
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    maxPacketCount_ = maxPacketCount;
    cond_.notify_one();
}

#pragma endregion