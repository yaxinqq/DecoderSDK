#include "include/decodersdk/decoder_controller.h"

#include "decoder_controller.h"

namespace decoder_sdk {
DecoderController::DecoderController() : impl_{new internal::DecoderController}
{
}

DecoderController::~DecoderController()
{
}

bool DecoderController::open(const std::string &url, const Config &config)
{
    return impl_ ? impl_->open(url, config) : false;
}

void DecoderController::openAsync(const std::string &url, const Config &config,
                                  AsyncOpenCallback callback)
{
    if (!impl_)
        return;

    impl_->openAsync(url, config, callback);
}

bool DecoderController::close()
{
    return impl_ ? impl_->close() : false;
}

bool DecoderController::pause()
{
    return impl_ ? impl_->pause() : false;
}

bool DecoderController::resume()
{
    return impl_ ? impl_->resume() : false;
}

bool DecoderController::startDecode()
{
    return impl_ ? impl_->startDecode() : false;
}

bool DecoderController::stopDecode()
{
    return impl_ ? impl_->stopDecode() : false;
}

bool DecoderController::isDecodeStopped() const
{
    return impl_ ? impl_->isDecodeStopped() : false;
}

bool DecoderController::isDecodePaused() const
{
    return impl_ ? impl_->isDecodePaused() : false;
}

bool DecoderController::seek(double position)
{
    return impl_ ? impl_->seek(position) : false;
}

bool DecoderController::setSpeed(double speed)
{
    return impl_ ? impl_->setSpeed(speed) : false;
}

FrameQueue DecoderController::videoQueue()
{
    if (!impl_)
        return FrameQueue(nullptr);

    return FrameQueue(impl_->videoQueue().get());
}

FrameQueue DecoderController::audioQueue()
{
    if (!impl_)
        return FrameQueue(nullptr);

    return FrameQueue(impl_->audioQueue().get());
}

void DecoderController::setMasterClock(MasterClock type)
{
    if (!impl_)
        return;

    impl_->setMasterClock(type);
}

double DecoderController::getVideoFrameRate() const
{
    return impl_ ? impl_->getVideoFrameRate() : 0.0;
}

void DecoderController::setFrameRateControl(bool enable)
{
    if (!impl_)
        return;

    impl_->setFrameRateControl(enable);
}

bool DecoderController::isFrameRateControlEnabled() const
{
    return impl_ ? impl_->isFrameRateControlEnabled() : false;
}

double DecoderController::curSpeed() const
{
    return impl_ ? impl_->curSpeed() : 0.0;
}

bool DecoderController::startRecording(const std::string &outputPath)
{
    return impl_ ? impl_->startRecording(outputPath) : false;
}

bool DecoderController::stopRecording()
{
    return impl_ ? impl_->stopRecording() : false;
}

bool DecoderController::isRecording() const
{
    return impl_ ? impl_->isRecording() : false;
}

void DecoderController::cancelAsyncOpen()
{
    if (!impl_)
        return;

    impl_->cancelAsyncOpen();
}

bool DecoderController::isAsyncOpenInProgress() const
{
    return impl_ ? impl_->isAsyncOpenInProgress() : false;
}

GlobalEventListenerHandle DecoderController::addGlobalEventListener(
    const std::function<EventCallback> &callback)
{
    if (!impl_)
        return GlobalEventListenerHandle();

    return impl_->addGlobalEventListener(callback);
}

bool DecoderController::removeGlobalEventListener(const GlobalEventListenerHandle &handle)
{
    if (!impl_)
        return false;

    return impl_->removeGlobalEventListener(handle);
}

EventListenerHandle DecoderController::addEventListener(
    EventType eventType, const std::function<EventCallback> &callback)
{
    if (!impl_)
        return EventListenerHandle();

    return impl_->addEventListener(eventType, callback);
}

bool DecoderController::removeEventListener(EventType eventType, EventListenerHandle handle)
{
    if (!impl_)
        return false;

    return impl_->removeEventListener(eventType, handle);
}

bool DecoderController::isReconnecting() const
{
    return impl_ ? impl_->isReconnecting() : false;
}

PreBufferState DecoderController::getPreBufferState() const
{
    return impl_ ? impl_->getPreBufferState() : PreBufferState::kDisabled;
}

PreBufferProgress DecoderController::getPreBufferProgress() const
{
    return impl_ ? impl_->getPreBufferProgress() : PreBufferProgress();
}

bool DecoderController::isRealTimeUrl() const
{
    return impl_ ? impl_->isRealTimeUrl() : false;
}
} // namespace decoder_sdk