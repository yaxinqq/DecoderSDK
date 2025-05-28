#include "FrameQueue.h"

#pragma region Frame
Frame::Frame()
    : frame_(nullptr),
      serial_(0),
      duration_(0),
      isInHardware_(false),
      isFlipV_(false),
      pts_(0.0)
{
    // 不在构造函数中分配内存，只在需要时分配
}

Frame::Frame(AVFrame* srcFrame)
    : frame_(nullptr),
      serial_(0),
      duration_(0),
      isInHardware_(false),
      isFlipV_(false),
      pts_(0.0)
{
    if (srcFrame) {
        ensureAllocated();
#ifdef USE_VAAPI
        if (!copyFrmae(srcFrame)) {
#else
        if (av_frame_ref(frame_, srcFrame) != 0) {
#endif
            release();
        }
    }
}

Frame::Frame(const Frame& other)
    : frame_(nullptr),
      serial_(other.serial_),
      duration_(other.duration_),
      isInHardware_(other.isInHardware_),
      isFlipV_(other.isFlipV_),
      pts_(other.pts_)
{
    if (other.frame_) {
        ensureAllocated();
#ifdef USE_VAAPI
        if (!copyFrmae(other.frame_)) {
#else
        if (av_frame_ref(frame_, other.frame_) != 0) {
#endif
            release();
        }
    }
}

Frame& Frame::operator=(const Frame& other)
{
    if (this != &other) {
        release();
        serial_ = other.serial_;
        duration_ = other.duration_;
        isInHardware_ = other.isInHardware_;
        isFlipV_ = other.isFlipV_;
        pts_ = other.pts_;

        if (other.frame_) {
            ensureAllocated();
#ifdef USE_VAAPI
            if (!copyFrmae(other.frame_)) {
#else
            if (av_frame_ref(frame_, other.frame_) != 0) {
#endif
                release();
            }
        }
    }
    return *this;
}

// 移动构造函数
Frame::Frame(Frame&& other) noexcept
    : frame_(other.frame_),
      serial_(other.serial_),
      duration_(other.duration_),
      isInHardware_(other.isInHardware_),
      isFlipV_(other.isFlipV_),
      pts_(other.pts_)
{
    // 转移所有权，避免深拷贝
    other.frame_ = nullptr;
}

// 移动赋值运算符
Frame& Frame::operator=(Frame&& other) noexcept
{
    if (this != &other) {
        release();

        // 转移所有权
        frame_ = other.frame_;
        serial_ = other.serial_;
        duration_ = other.duration_;
        isInHardware_ = other.isInHardware_;
        isFlipV_ = other.isFlipV_;
        pts_ = other.pts_;

        other.frame_ = nullptr;
    }
    return *this;
}

Frame::~Frame()
{
    release();
}

AVFrame* Frame::get() const
{
    return frame_;
}

bool Frame::isValid() const
{
    return frame_ != nullptr;
}

void Frame::ensureAllocated()
{
    if (!frame_) {
        frame_ = av_frame_alloc();
    }
}

int Frame::serial() const
{
    return serial_;
}

void Frame::setSerial(int serial)
{
    serial_ = serial;
}

double Frame::duration() const
{
    return duration_;
}

void Frame::setDuration(double duration)
{
    duration_ = duration;
}

bool Frame::isInHardware() const
{
    return isInHardware_;
}

void Frame::setIsInHardware(bool isInHardware)
{
    isInHardware_ = isInHardware;
}

bool Frame::isFlipV() const
{
    return isFlipV_;
}

void Frame::setIsFlipV(bool isFlipV)
{
    isFlipV_ = isFlipV;
}

void Frame::setPts(double pts)
{
    pts_ = pts;
}

double Frame::pts() const
{
    return pts_;
}

void Frame::unref()
{
    if (frame_) {
        av_frame_unref(frame_);

#ifdef USE_VAAPI
        if (frame_->data[0]) {
            free(frame_->data[0]);
        }
        if (frame_->data[1]) {
            free(frame_->data[1]);
        }
#endif
    }
}

void Frame::release()
{
    if (frame_) {
        unref();
        av_frame_free(&frame_);
        frame_ = nullptr;
    }
}

#ifdef USE_VAAPI
bool Frame::copyFrmae(AVFrame* srcFrame)
{
    if (!srcFrame)
        return false;

    frame_->width = srcFrame->width;
    frame_->height = srcFrame->height;
    frame_->format = srcFrame->format;

    egl::RDDmaBufExternalMemory yBuf =
        *reinterpret_cast<egl::RDDmaBufExternalMemory*>(srcFrame->data[0]);
    frame_->data[0] = (uint8_t*)malloc(sizeof(egl::RDDmaBufExternalMemory));
    memcpy(frame_->data[0], &(yBuf), sizeof(egl::RDDmaBufExternalMemory));

    egl::RDDmaBufExternalMemory uvBuf =
        *reinterpret_cast<egl::RDDmaBufExternalMemory*>(srcFrame->data[1]);
    frame_->data[1] = (uint8_t*)malloc(sizeof(egl::RDDmaBufExternalMemory));
    memcpy(frame_->data[1], &(uvBuf), sizeof(egl::RDDmaBufExternalMemory));

    return true;
}
#endif

#pragma endregion

#pragma region FrameQueue

FrameQueue::FrameQueue(int maxSize, bool keepLast)
    : rindex_{0},
      rindexShown_{0},
      windex_{0},
      size_{0},
      maxSize_{maxSize},
      keepLast_{keepLast}
{
    for (int i = 0; i < maxSize_; ++i) {
        Frame frame;
        frame.ensureAllocated();
        queue_.push_back(std::move(frame));
    }
}

FrameQueue::~FrameQueue()
{
    while (!queue_.empty()) {
        queue_.pop_back();
    }
}

void FrameQueue::setAbortStatus(bool abort)
{
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = abort;
    cond_.notify_all();
}

void FrameQueue::setSerial(int serial)
{
    std::lock_guard<std::mutex> lock(mutex_);
    serial_ = serial;
}

void FrameQueue::awakeCond()
{
    std::lock_guard<std::mutex> lock(mutex_);
    cond_.notify_all();
}

Frame* FrameQueue::peekWritable()
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this]() { return size_ < maxSize_ || aborted_; });
    }

    if (aborted_)
        return nullptr;

    return &queue_[windex_];
}

Frame* FrameQueue::peekReadable()
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock,
                   [this]() { return size_ - rindexShown_ > 0 || aborted_; });
    }

    if (aborted_)
        return nullptr;

    return &queue_[(rindex_ + rindexShown_) % maxSize_];
}

Frame* FrameQueue::peek()
{
    return &queue_[(rindex_ + rindexShown_) % maxSize_];
}

Frame* FrameQueue::peekNext()
{
    return &queue_[(rindex_ + rindexShown_ + 1) % maxSize_];
}

Frame* FrameQueue::peekLast()
{
    return &queue_[rindex_];
}

int FrameQueue::push()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (++windex_ == maxSize_)
        windex_ = 0;

    size_++;
    cond_.notify_one();
    return 0;
}

int FrameQueue::pop()
{
    std::unique_lock<std::mutex> lock(mutex_);

    // 如果队列为空，且没有中止请求，等待直到队列中有帧
    cond_.wait(lock, [this]() { return size_ > 0 || aborted_; });

    // 如果队列中有帧，则弹出并更新 rindex
    if (size_ > 0) {
        queue_[rindex_].unref();
        rindex_ = (rindex_ + 1) % maxSize_;
        --size_;
        cond_.notify_one();  // 唤醒等待线程
        return 0;            // 成功
    }

    return -1;  // 队列为空，未能成功弹出帧
}

void FrameQueue::next()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (keepLast_ && rindexShown_ != 0) {
        rindexShown_ = 1;
        return;
    }
    queue_[rindex_].unref();
    size_--;
    cond_.notify_one();
}

bool FrameQueue::popFrame(Frame& frame, int timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (timeout == 0) {
        if (size_ <= 0)
            return false;
        else {
            frame = queue_[rindex_];
            queue_[rindex_].unref();
            rindex_ = (rindex_ + 1) % maxSize_;
            --size_;
            cond_.notify_one();  // 唤醒其他线程
            return true;         // 成功
        }
    }

    if (timeout < 0) {
        // 等待直到队列中有帧
        cond_.wait(lock, [this]() { return size_ > 0 || aborted_; });
    } else {
        // 等待指定的超时时间
        if (cond_.wait_for(lock, std::chrono::milliseconds(timeout), [this]() {
                return size_ > 0 || aborted_;
            }) == false) {
            return false;  // 超时
        }
    }

    // 如果队列中有帧，弹出并返回该帧
    if (size_ > 0) {
        frame = queue_[rindex_];
        rindex_ = (rindex_ + 1) % maxSize_;
        --size_;
        cond_.notify_one();  // 唤醒其他线程
        return true;         // 成功
    }

    return false;  // 队列为空或中止请求
}

int FrameQueue::remainingCount() const
{
    return size_ - rindexShown_;
}

int FrameQueue::lastFramePts()
{
    Frame* frame = &queue_[rindex_];

    if (rindexShown_ && frame->serial() == serial_)
        return frame->get() ? frame->get()->pkt_pos : -1;
    else
        return -1;
}

void FrameQueue::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 清空所有帧
    for (int i = 0; i < size_; i++) {
        int idx = (rindex_ + i) % maxSize_;
        queue_[idx].unref();
    }

    // 重置索引和计数器
    rindex_ = 0;
    windex_ = 0;
    rindexShown_ = 0;
    size_ = 0;

    // 通知所有等待的线程
    cond_.notify_all();
}

#pragma endregion