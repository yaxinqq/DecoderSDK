#include "FrameQueue.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

#pragma region Frame
Frame::Frame()
    : frame_(av_frame_alloc())
    , serial_{0}
    , duration_{0}
    , isInHardware_{false}
    , isFlipV_{false}
{
}

Frame::Frame(AVFrame *srcFrame)
    : frame_(av_frame_alloc())
{
#ifdef USE_VAAPI
    if (!copyFrmae(srcFrame)) {
#else
    if (!srcFrame || av_frame_ref(frame_, srcFrame) != 0) {
#endif
        release();
    }
}

Frame::Frame(const Frame &other)
    : frame_(av_frame_alloc())
{
#ifdef USE_VAAPI
    if (!copyFrmae(other.frame_)) {
#else
    if (!other.frame_ || av_frame_ref(frame_, other.frame_) != 0) {
#endif
        release();
    } else {
        serial_ = other.serial_;
        duration_ = other.duration_;
        isInHardware_ = other.isInHardware_;
        isFlipV_ = other.isFlipV_;
    }


}

Frame &Frame::operator=(const Frame &other)
{
    if (this != &other) {
        release();
        frame_ = av_frame_alloc();
        if (other.frame_) {
        #ifdef USE_VAAPI
            if (!copyFrmae(other.frame_)) {
        #else
            if (av_frame_ref(frame_, other.frame_) != 0) {
        #endif
                release();
            } else {
                serial_ = other.serial_;
                duration_ = other.duration_;
                isInHardware_ = other.isInHardware_;
                isFlipV_ = other.isFlipV_;
            }
        }
    }
    return *this;
}

Frame::~Frame()
{
    release();
}

AVFrame *Frame::get() const
{
    return frame_;
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

void Frame::unref()
{
    av_frame_unref(frame_);
}

void Frame::release()
{
    if (frame_) {
#ifdef USE_VAAPI
        if (frame_->data[0]) {
            free(frame_->data[0]);
        }
        if (frame_->data[1]) {
            free(frame_->data[1]);
        }
#endif
        av_frame_free(&frame_);
        frame_ = nullptr;
    }
}

#ifdef USE_VAAPI
bool Frame::copyFrmae(AVFrame *srcFrame)
{
    if (!srcFrame)
        return false;

    frame_->width = srcFrame->width;
    frame_->height = srcFrame->height;
    frame_->format = srcFrame->format;

    egl::RDDmaBufExternalMemory yBuf = *reinterpret_cast<egl::RDDmaBufExternalMemory*>(srcFrame->data[0]);
    frame_->data[0] = (uint8_t*)malloc(sizeof(egl::RDDmaBufExternalMemory));
    memcpy(frame_->data[0], &(yBuf), sizeof(egl::RDDmaBufExternalMemory));

    egl::RDDmaBufExternalMemory uvBuf = *reinterpret_cast<egl::RDDmaBufExternalMemory*>(srcFrame->data[1]);
    frame_->data[1] = (uint8_t*)malloc(sizeof(egl::RDDmaBufExternalMemory));
    memcpy(frame_->data[1], &(uvBuf), sizeof(egl::RDDmaBufExternalMemory));

    return true;
}
#endif

#pragma endregion


#pragma region FrameQueue

FrameQueue::FrameQueue(int maxSize, bool keepLast)
    : rindex_{0}
    , rindexShown_{0}
    , windex_{0}
    , size_{0}
    , maxSize_{maxSize}
    , keepLast_{keepLast}
{
    for (int i = 0; i < maxSize_; ++i) {
        queue_.push_back({});
    }
}

FrameQueue::~FrameQueue()
{
    while(queue_.empty()) {
        queue_.pop_back();
    }
}

void FrameQueue::setAbortStatus(bool abort)
{
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = abort;
}

void FrameQueue::setSerial(int serial)
{
    std::lock_guard<std::mutex> lock(mutex_);
    serial_ = serial;
}

void FrameQueue::awakeCond()
{
    std::lock_guard<std::mutex> lock(mutex_);
    cond_.notify_one();
}

Frame *FrameQueue::peekWritable()
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this](){
            return size_ >= maxSize_ && !aborted_;
        });
    }

    if (aborted_)
        return nullptr;

    return &queue_[windex_];
}

Frame *FrameQueue::peekReadable()
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this](){
            return size_ - rindexShown_ <= 0 && !aborted_;
        });
    }

    if (aborted_)
        return nullptr;

    return &queue_[(rindex_ + rindexShown_) % maxSize_];
}

Frame *FrameQueue::peek()
{
    return &queue_[(rindex_ + rindexShown_) % maxSize_];
}

Frame *FrameQueue::peekNext()
{
    return &queue_[(rindex_ + rindexShown_ + 1) % maxSize_];
}

Frame *FrameQueue::peekLast()
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
}

int FrameQueue::pop()
{
    std::unique_lock<std::mutex> lock(mutex_);

    // 如果队列为空，且没有中止请求，等待直到队列中有帧
    cond_.wait(lock, [this]() { return size_ > 0 || aborted_; });

    // 如果队列中有帧，则弹出并更新 rindex
    if (size_ > 0) {
        rindex_ = (rindex_ + 1) % maxSize_;
        --size_;
        cond_.notify_one();  // 唤醒等待线程
        return 0;  // 成功
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

bool FrameQueue::popFrame(Frame &frame, int timeout = -1)
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (timeout < 0) {
        // 等待直到队列中有帧
        cond_.wait(lock, [this]() { return size_ > 0 || aborted_; });
    } else {
        // 等待指定的超时时间
        if (cond_.wait_for(lock, std::chrono::milliseconds(timeout), [this]() { return size_ > 0 || aborted_; }) == false) {
            return false;  // 超时
        }
    }

    // 如果队列中有帧，弹出并返回该帧
    if (size_ > 0) {
        frame = queue_[rindex_];
        rindex_ = (rindex_ + 1) % maxSize_;
        --size_;
        cond_.notify_one();  // 唤醒其他线程
        return true;  // 成功
    }

    return false;  // 队列为空或中止请求
}

int FrameQueue::remainingCount() const
{
    return size_ - rindexShown_;
}

int FrameQueue::lastFramePts()
{
    Frame *frame = &queue_[rindex_];

    if (rindexShown_ && frame->serial() == serial_)
        return frame->get() ? frame->get()->pkt_pos : -1;
    else
        return -1;
}

#pragma endregion

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END