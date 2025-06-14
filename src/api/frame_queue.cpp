#include "include/decodersdk/frame_queue.h"

#include "base/frame_queue.h"

namespace decoder_sdk {

class FrameQueueImpl {
public:
    explicit FrameQueueImpl(internal::FrameQueue *queue);
    FrameQueueImpl(const FrameQueueImpl &other);
    ~FrameQueueImpl();

    internal::FrameQueue *get() const;

private:
    std::shared_ptr<internal::FrameQueue> queue_;
};

FrameQueueImpl::FrameQueueImpl(internal::FrameQueue *queue)
{
    if (queue) {
        queue_ = queue->shared_from_this();
    }
}

FrameQueueImpl::FrameQueueImpl(const FrameQueueImpl &other) : queue_(other.queue_)
{
}

FrameQueueImpl::~FrameQueueImpl()
{
}

internal::FrameQueue *FrameQueueImpl::get() const
{
    return queue_.get();
}

FrameQueue::FrameQueue(internal::FrameQueue *queue) : impl_{new FrameQueueImpl{queue}}
{
}

FrameQueue::~FrameQueue()
{
}

FrameQueue::FrameQueue(const FrameQueue &other)
{
    if (other.impl_) {
        impl_ = std::make_unique<FrameQueueImpl>(*other.impl_);
    }
}

FrameQueue &FrameQueue::operator=(const FrameQueue &other)
{
    if (this != &other) {
        if (other.impl_) {
            impl_ = std::make_unique<FrameQueueImpl>(*other.impl_);
        } else {
            impl_.reset();
        }
    }
    return *this;
}

bool FrameQueue::pop(Frame &frame, int timeout)
{
    if (!impl_ || !impl_->get()) {
        return false;
    }

    std::unique_ptr<internal::Frame> internalFrame{new internal::Frame()};
    impl_->get()->pop(*internalFrame, timeout);
    Frame res{std::move(internalFrame)};
    std::swap(frame, res);
    return true;
}

bool FrameQueue::tryPop(Frame &frame)
{
    return pop(frame, 0);
}

bool FrameQueue::empty() const
{
    return (!impl_ && !impl_->get()) ? impl_->get()->empty() : true;
}

bool FrameQueue::full() const
{
    return (!impl_ && !impl_->get()) ? impl_->get()->full() : false;
}

int FrameQueue::size() const
{
    return (!impl_ && !impl_->get()) ? impl_->get()->size() : 0;
}

int FrameQueue::capacity() const
{
    return (!impl_ && !impl_->get()) ? impl_->get()->capacity() : 0;
}

int FrameQueue::remainingCount() const
{
    return (!impl_ && !impl_->get()) ? impl_->get()->remainingCount() : 0;
}
} // namespace decoder_sdk