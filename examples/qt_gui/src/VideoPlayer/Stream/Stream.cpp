#include "Stream.h"
#include "../Player/VideoPlayerImpl.h"
#include "InternalUtils.h"
#include "../Base/CommonDef.h"

#include "decodersdk/common_define.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QMutexLocker>
#include <QOffscreenSurface>
#include <QThread>

// 用来调试解码器状态的宏
#define DEBUG_DECODER 0

#if DEBUG_DECODER
// 当前存活的解码器数量
static std::atomic_int g_existingDecoder = 0;
// 当前正在解码的解码器数量
static std::atomic_int g_decodingDecoder = 0;
#endif

namespace {
Stream::ClockSourceType determineClockSource(const decoder_sdk::StreamInfo &info)
{
    if (info.isRealtime) {
        return Stream::ClockSourceType::kNone;
    }

    if (info.audioInfo) {
        return Stream::ClockSourceType::kAudioMaster;
    }

    if (info.videoInfo) {
        return Stream::ClockSourceType::kExternalMaster;
    }

    return Stream::ClockSourceType::kNone;
}
} // namespace

#pragma region StreamPipeline
StreamPipeline::StreamPipeline(const QString &key, QObject *parent /*= nullptr*/)
    : QThread(parent)
    , key_{ key }
{
    initDecoder(key);
    initRenderer(key);

#if DEBUG_DECODER
    g_existingDecoder.fetch_add(1);
    qDebug()
        << QStringLiteral("****** existing decoder count: %1 ******").arg(g_existingDecoder.load());
#endif
}

StreamPipeline::~StreamPipeline()
{
    if (isRunning()) {
        requestInterruption();
        condition_.notify_all(); // 防止run循环中还有休眠的条件变量，这里唤醒全部
        quit();

        wait();
    }

    // 销毁渲染器
    if (renderer_) {
        delete renderer_;
        renderer_ = nullptr;
    }

    // 销毁解码器
    if (decoder_) {
        delete decoder_;
        decoder_ = nullptr;
    }

#if DEBUG_DECODER
    g_existingDecoder.fetch_sub(1);
    qDebug()
        << QStringLiteral("****** existing decoder count: %1 ******").arg(g_existingDecoder.load());
#endif
}

void StreamPipeline::appendTask(DecodeWorker::Task task)
{
    std::lock_guard l(mutex_);

    // 判断任务是否需要执行，不需要执行，则直接返回
    if (!shouldExecuteTask(task))
        return;

    // 获得队列中的第一个任务，如果是关闭，则此时不应该再接受其它任务
    if (!tasks_.isEmpty() && tasks_.front() == DecodeWorker::Task::kClose) {
        // 唤醒消费者
        condition_.notify_one();
        // 返回
        return;
    }

    // 清空之前的任务，只保留最新的
    tasks_.clear();
    tasks_.append(task);

    // 如果当前的任务是关闭，则通知外部
    if (task == DecodeWorker::Task::kClose) {
        decoderPreparingToClose_ = true;
    }

    // 唤醒消费者
    condition_.notify_one();
}

void StreamPipeline::open(const QString &url, const decoder_sdk::DecoderConfig &config)
{
    // 只开启一次，后续加进来的player，会调用resume，防止当前解码器正在暂停
    if (!once_.load()) {
        once_.store(true);
        emit requestToOpenAsync(url, config);
    } else {
        appendTask(DecodeWorker::Task::kResume);

        // 广播流信息和解码器信息，通知后续加入的player
        emit requestToBroadcastStreamAndDecoderInfo();
    }
}

bool StreamPipeline::isRecodering() const
{
    return isRecording_;
}

bool StreamPipeline::decoderPreparingToClose() const
{
    return decoderPreparingToClose_;
}

QString StreamPipeline::key() const
{
    return key_;
}

void StreamPipeline::registerPlayer(VideoPlayerImpl *player)
{
    if (!player || refPlayers_.contains(player) || decoder_.isNull() || renderer_.isNull())
        return;

    connect(this, &StreamPipeline::eventUpdated, player, &VideoPlayerImpl::onDecoderEventChanged,
            Qt::UniqueConnection);
    connect(this, &StreamPipeline::streamInfoUpdated, player, &VideoPlayerImpl::onStreamInfoUpdated,
            Qt::UniqueConnection);
    connect(this, &StreamPipeline::decoderInfoUpdated, player, &VideoPlayerImpl::onDecoderInfoUpdated,
            Qt::UniqueConnection);

    connect(this, &StreamPipeline::renderNameChanged, player, &VideoPlayerImpl::onRendererNameChanged,
            Qt::UniqueConnection);
    connect(this, &StreamPipeline::textureReady, player, &VideoPlayerImpl::onTextureReady,
            Qt::UniqueConnection);
    connect(this, &StreamPipeline::displayRendererReady, player, &VideoPlayerImpl::onDisplayRendererReady,
            Qt::UniqueConnection);
    connect(this, &StreamPipeline::displayRendererAboutToDestroy, player, &VideoPlayerImpl::onDisplayRendererAboutToDestroy,
            Qt::UniqueConnection);

    // 创建player对应的displayRenderer
    renderer_->addDisplayRenderer(player->id());

    refPlayers_ << player;
}

void StreamPipeline::unRegisterPlayer(VideoPlayerImpl *player)
{
    if (!player || !refPlayers_.contains(player) || decoder_.isNull() || renderer_.isNull())
        return;

    // 销毁player对应的displayRenderer
    renderer_->removeDisplayRenderer(player->id());

    disconnect(this, &StreamPipeline::eventUpdated, player,
               &VideoPlayerImpl::onDecoderEventChanged);
    disconnect(this, &StreamPipeline::streamInfoUpdated, player,
               &VideoPlayerImpl::onStreamInfoUpdated);
    disconnect(this, &StreamPipeline::decoderInfoUpdated, player,
               &VideoPlayerImpl::onDecoderInfoUpdated);

    disconnect(this, &StreamPipeline::renderNameChanged, player, &VideoPlayerImpl::onRendererNameChanged);
    disconnect(this, &StreamPipeline::textureReady, player, &VideoPlayerImpl::onTextureReady);
    disconnect(this, &StreamPipeline::displayRendererReady, player, &VideoPlayerImpl::onDisplayRendererReady);
    disconnect(this, &StreamPipeline::displayRendererAboutToDestroy, player, &VideoPlayerImpl::onDisplayRendererAboutToDestroy);
    refPlayers_.removeOne(player);
}

bool StreamPipeline::registerEventCallback(decoder_sdk::EventType type,
                                           std::function<void(const QString &url, decoder_sdk::EventType type,
                                                              const std::shared_ptr<decoder_sdk::EventArgs> &event)>
                                               cb)
{
    if (!eventCallbackMap_.contains(type)) {
        eventCallbackMap_.insert(type, {});
    }

    eventCallbackMap_[type].append(std::move(cb));
    return true; 
}

void StreamPipeline::run()
{
    // 如果出现"未执行task就退出循环的情况  可能需要修改while的跳出条件为：!tasks_.isEmpty() &&
    // isInterruptionRequested()"
    while (!isInterruptionRequested()) {
        std::unique_lock l(mutex_);
        condition_.wait(l, [this]() { return !tasks_.isEmpty() || isInterruptionRequested(); });

        if (isInterruptionRequested() && tasks_.isEmpty())
            break;

        const auto t = tasks_.takeFirst();
        l.unlock(); 

        emit requestToDoTask(t);
    }
}

void StreamPipeline::setRecordingStatus(bool status)
{
    isRecording_ = status;
}

void StreamPipeline::onOpenResultReady(bool res, const QString &errorMsg)
{
    // 这里不管成功还是失败，都开启事件循环，不能影响后续任务的接收
    // 开启事件循环
    start();
}

void StreamPipeline::onDecoderRequestToDelete()
{
    // 销毁渲染器
    if (renderer_) {
        renderer_->deleteLater();
    }

    // 销毁解码器
    if (decoder_) {
        decoder_->deleteLater();
    }

    // 向外通知
    emit aboutToDelete(key_);

    // 销毁自身
    deleteLater();
}

void StreamPipeline::onEventUpdated(const QString &url, decoder_sdk::EventType type,
                                    const std::shared_ptr<decoder_sdk::EventArgs> &event)
{
    switch (type) {
        case decoder_sdk::EventType::kStreamOpened:
        {
            // 根据流的音频、视频是否参与解码以及是否为实时流，来设置render的主时钟类型
            Stream::ClockSourceType clockType = Stream::ClockSourceType::kNone;

            if (auto *streamEvent = dynamic_cast<decoder_sdk::StreamEventArgs *>(event.get());
                streamEvent && streamEvent->streamInfo) {
                clockType = determineClockSource(*streamEvent->streamInfo);
            }

            renderer_->setClockSourceType(clockType);
            break;
        }
        case decoder_sdk::EventType::kDecodeStarted:
        case decoder_sdk::EventType::kDecodeFirstFrame:
            // 开启渲染器
            renderer_->start();
            break;
        case decoder_sdk::EventType::kDecodePaused:
        case decoder_sdk::EventType::kDecodeStopped:
            // 关闭渲染器
            renderer_->stop();
            break;
        case decoder_sdk::EventType::kSeekSuccess:
            renderer_->resetTimeline();
            break;
        case decoder_sdk::EventType::kRecordingStarted:
            // 设置当前的录像状态
            setRecordingStatus(true);
            break;
        case decoder_sdk::EventType::kRecordingStopped:
            // 设置当前的录像状态
            setRecordingStatus(false);
            break;
        case decoder_sdk::EventType::kRecordingError:
            // 如果当前没有player和这个Worker绑定，执行录像错误的默认操作
            if (refPlayers_.isEmpty()) {
                emit requestToStopRecording();
            }
            break;
        default:
            break;
    }

    emit eventUpdated(url, type, event);

    // 执行回调
    if (eventCallbackMap_.contains(type)) {
        const auto cbs = eventCallbackMap_.take(type);
        for (const auto &cb : cbs) {
            if (!cb)
                continue;

            cb(url, type, event);
        }
    }
}

void StreamPipeline::initDecoder(const QString &key)
{
    // 创建解码器
    decoder_ = new DecodeWorker(key);

    // ============== Worker => Decoder的信号 ============== //
    connect(this, &StreamPipeline::requestToStartRecoding, decoder_,
            &DecodeWorker::startRecoding);
    connect(this, &StreamPipeline::requestToStopRecording, decoder_,
            &DecodeWorker::stopRecording);
    connect(this, &StreamPipeline::requestToSeek, decoder_, &DecodeWorker::seek);
    connect(this, &StreamPipeline::requestToSetSpeed, decoder_, &DecodeWorker::setSpeed);
    connect(this, &StreamPipeline::requestToSetLoopMode, decoder_, &DecodeWorker::setLoopMode);
    connect(this, &StreamPipeline::requestToResetLoopCount, decoder_, &DecodeWorker::resetLoopCount);
    connect(this, &StreamPipeline::requestToOpenAsync, decoder_, &DecodeWorker::openAsync);
    connect(this, &StreamPipeline::requestToBroadcastStreamAndDecoderInfo, decoder_, &DecodeWorker::broadcastStreamAndDecoderInfo);
    connect(this, &StreamPipeline::requestToDoTask, decoder_, &DecodeWorker::doTask, Qt::BlockingQueuedConnection);
    // ===================================================== //

    // ============== Decoder => Worker的信号 ============== //
    connect(decoder_, &DecodeWorker::openResultReady, this, &StreamPipeline::onOpenResultReady);
    connect(decoder_, &DecodeWorker::requestToDelete, this, &StreamPipeline::onDecoderRequestToDelete);

    connect(decoder_, &DecodeWorker::eventUpdated, this, &StreamPipeline::onEventUpdated);
    connect(decoder_, &DecodeWorker::streamInfoUpdated, this, &StreamPipeline::streamInfoUpdated);
    connect(decoder_, &DecodeWorker::decoderInfoUpdated, this, &StreamPipeline::decoderInfoUpdated);
    // ===================================================== //
}

void StreamPipeline::initRenderer(const QString &threadNameSuffix)
{
    // 渲染表面
    auto renderSurface = new QOffscreenSurface(nullptr, this);
    renderSurface->create();

    // 创建渲染器
    renderer_ = new RenderWorker(renderSurface, threadNameSuffix);
    // 设置对应的解码器
    renderer_->setDecoder(decoder_.data());

    // ============== Worker => Decoder的信号 ============== //
    connect(this, &StreamPipeline::requestToSetSpeed, renderer_, &RenderWorker::setSpeed);
    // ===================================================== //

    // ============== Renderer => Worker的信号 ============== //
    connect(renderer_, &RenderWorker::renderNameChanged, this, &StreamPipeline::renderNameChanged);
    connect(renderer_, &RenderWorker::textureReady, this, &StreamPipeline::textureReady);

    connect(renderer_, &RenderWorker::displayRendererReady, this, &StreamPipeline::displayRendererReady);
    connect(renderer_, &RenderWorker::displayRendererAboutToDestroy, this, &StreamPipeline::displayRendererAboutToDestroy);
    // ===================================================== //
}

bool StreamPipeline::shouldExecuteTask(DecodeWorker::Task task) const
{
    // 如果当前关联的播放器为空，则总是可以执行任务的
    if (refPlayers_.isEmpty())
        return true;

    switch (task) {
        case DecodeWorker::Task::kPause:
            // 如果有正在播放、或即将播放的关联播放器，则不应该处理暂停命令
            for (auto *const player : refPlayers_) {
                if (player->playerState() == Stream::PlayerState::Playing ||
                    player->playerState() == Stream::PlayerState::Resume) {
                    return false;
                }
            }

            // 所有播放器都没有播放时，才可以暂停
            return true;
        case DecodeWorker::Task::kResume:
            // 恢复播放总是可以执行的
            return true;
        case DecodeWorker::Task::kClose:
            // 只有当前没有任何关联播放器时，才能关闭
            return refPlayers_.isEmpty();
        default:
            break;
    }

    return false;
}

#pragma endregion
