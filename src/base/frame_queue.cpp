#include "frame_queue.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

FrameQueue::FrameQueue(int maxSize, bool keepLast, bool autoInit)
    : head_(0),
      tail_(0),
      size_(0),
      maxSize_(maxSize),
      keepLast_(keepLast),
      pendingWriteIndex_(-1),
      serial_(0),
      aborted_(false)
{
    if (autoInit) {
        init();
    }
}

FrameQueue::~FrameQueue()
{
    setAbortStatus(true);
    clear();
}

bool FrameQueue::push(const Frame &frame, int timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);

    // 检查是否有空间
    auto hasSpace = [this]() { return size_ < maxSize_ || aborted_.load(); };

    if (timeout == 0) {
        if (!hasSpace()) {
            return false;
        }
    } else if (timeout > 0) {
        if (!cond_.wait_for(lock, std::chrono::milliseconds(timeout), hasSpace)) {
            return false;
        }
    } else {
        cond_.wait(lock, hasSpace);
    }

    if (aborted_.load() || queue_.empty()) {
        return false;
    }

    return pushInternal(frame);
}

bool FrameQueue::pop(Frame &frame, int timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);

    // 等待数据可用
    if (!waitForData(lock, timeout)) {
        return shouldReturnLastFrame() ? (frame = queue_[head_], true) : false;
    }

    if (aborted_.load() || queue_.empty()) {
        return false;
    }

    // 处理keepLast情况或正常弹出
    if (shouldReturnLastFrame()) {
        frame = queue_[head_];
        return true;
    }

    return popInternal(frame);
}

bool FrameQueue::tryPop(Frame &frame)
{
    return pop(frame, 0);
}

Frame *FrameQueue::getWritableFrame(int timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);

    auto hasSpace = [this]() { return size_ < maxSize_ || aborted_.load(); };

    if (timeout == 0) {
        if (!hasSpace()) {
            return nullptr;
        }
    } else if (timeout > 0) {
        if (!cond_.wait_for(lock, std::chrono::milliseconds(timeout), hasSpace)) {
            return nullptr;
        }
    } else {
        cond_.wait(lock, hasSpace);
    }

    if (aborted_.load() || size_ >= maxSize_ || queue_.empty()) {
        return nullptr;
    }

    pendingWriteIndex_ = tail_;
    return &queue_[pendingWriteIndex_];
}

bool FrameQueue::commitFrame()
{
    std::lock_guard<std::mutex> lock(mutex_);

    tail_ = (tail_ + 1) % maxSize_;
    size_++;
    pendingWriteIndex_ = -1;

    notifyWaiters();
    return true;
}

bool FrameQueue::empty() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return size_ == 0;
}

bool FrameQueue::full() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return size_ >= maxSize_;
}

int FrameQueue::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}

int FrameQueue::capacity() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return maxSize_;
}

int FrameQueue::remainingCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}

void FrameQueue::clear()
{
    if (head_ == tail_ && size_ == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 清空所有帧
    for (int i = 0; i < size_; ++i) {
        int idx = (head_ + i) % maxSize_;
        queue_[idx].unref();
    }

    head_ = 0;
    tail_ = 0;
    size_ = 0;
    pendingWriteIndex_ = -1;

    notifyWaiters();
}

void FrameQueue::setAbortStatus(bool abort)
{
    if (abort == aborted_.load()) {
        return;
    }

    aborted_.store(abort);
    if (abort) {
        std::lock_guard<std::mutex> lock(mutex_);
        notifyWaiters();
    }
}

void FrameQueue::setSerial(int serial)
{
    std::lock_guard<std::mutex> lock(mutex_);
    serial_ = serial;
}

int FrameQueue::serial() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return serial_;
}

void FrameQueue::setKeepLast(bool keepLast)
{
    std::lock_guard<std::mutex> lock(mutex_);
    keepLast_ = keepLast;
}

bool FrameQueue::isKeepLast() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return keepLast_;
}

bool FrameQueue::setMaxCount(int maxCount)
{
    if (maxCount <= 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (maxCount == maxSize_) {
        return true; // 容量没有变化
    }

    // 保存当前数据
    std::vector<Frame> tempFrames;
    tempFrames.reserve(size_);

    // 将当前队列中的帧移动到临时容器
    for (int i = 0; i < size_; ++i) {
        int idx = (head_ + i) % maxSize_;
        tempFrames.emplace_back(std::move(queue_[idx]));
    }

    // 重新分配队列
    queue_.clear();
    queue_.resize(maxCount);

    // 为新队列中的每个位置分配Frame
    for (int i = 0; i < maxCount; ++i) {
        queue_[i].ensureAllocated();
    }

    // 更新容量
    maxSize_ = maxCount;

    // 重新填充队列
    head_ = 0;
    tail_ = 0;
    size_ = 0;
    pendingWriteIndex_ = -1;

    // 将保存的帧重新放入队列（如果新容量允许）
    int framesToRestore = std::min(static_cast<int>(tempFrames.size()), maxCount);
    for (int i = 0; i < framesToRestore; ++i) {
        queue_[tail_] = std::move(tempFrames[i]);
        tail_ = (tail_ + 1) % maxSize_;
        size_++;
    }

    // 通知等待的线程
    notifyWaiters();

    return true;
}

void FrameQueue::uninit()
{
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    tail_ = 0;
    size_ = 0;
    pendingWriteIndex_ = -1;

    if (queue_.empty())
        return;

    // 清空所有帧
    for (int i = 0; i < queue_.size(); ++i) {
        queue_[i].release();
    }
    queue_.clear();

    notifyWaiters();
}

void FrameQueue::init()
{
    uninit();

    std::lock_guard<std::mutex> lock(mutex_);
    queue_.resize(maxSize_);
    for (int i = 0; i < maxSize_; ++i) {
        queue_[i].ensureAllocated();
    }
}

bool FrameQueue::pushInternal(const Frame &frame)
{
    // 如果队列满了，移除最旧的帧
    if (size_ == maxSize_) {
        queue_[head_].unref();
        head_ = (head_ + 1) % maxSize_;
        size_--;
    }

    queue_[tail_] = std::move(frame);
    tail_ = (tail_ + 1) % maxSize_;
    size_++;

    notifyWaiters();
    return true;
}

bool FrameQueue::popInternal(Frame &frame)
{
    if (size_ == 0) {
        return false;
    }

    frame = std::move(queue_[head_]);
    head_ = (head_ + 1) % maxSize_;
    size_--;

    notifyWaiters();
    return true;
}

void FrameQueue::notifyWaiters()
{
    cond_.notify_all();
}

bool FrameQueue::handleKeepLastCase(Frame &frame)
{
    if (keepLast_ && size_ == 1) {
        frame = queue_[head_]; // 复制最后一帧
        return true;
    }
    return false;
}

bool FrameQueue::canPop() const
{
    return keepLast_ ? size_ > 1 : size_ > 0;
}

bool FrameQueue::shouldReturnLastFrame() const
{
    return keepLast_ && size_ == 1;
}

bool FrameQueue::waitForData(std::unique_lock<std::mutex> &lock, int timeout)
{
    auto hasData = [this]() { return canPop() || aborted_.load(); };

    if (timeout == 0) {
        return hasData();
    } else if (timeout > 0) {
        return cond_.wait_for(lock, std::chrono::milliseconds(timeout), hasData);
    } else {
        cond_.wait(lock, hasData);
        return true;
    }
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END