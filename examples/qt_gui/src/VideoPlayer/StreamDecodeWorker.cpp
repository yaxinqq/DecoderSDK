#include "StreamDecodeWorker.h"
#include "CommonDef.h"
#include "VideoRender/VideoPlayerImpl.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMutexLocker>
#include <QThread>

// 用来调试解码器状态的宏
#define DEBUG_DECODER 0

#if DEBUG_DECODER
// 当前存活的解码器数量
static std::atomic_int g_existingDecoder = 0;
// 当前正在解码的解码器数量
static std::atomic_int g_decodingDecoder = 0;
#endif

#pragma region DecoderThread
DecoderThread::DecoderThread(StreamDecoder *const decoder, QObject *parent)
    : QThread(parent), pDecoder_{decoder}
{
}

DecoderThread::~DecoderThread()
{
}

void DecoderThread::run()
{
    while (pDecoder_ && !isInterruptionRequested()) {
        bool shouldSleep = true;
        {
            // 音频。需要验证音频帧有效
            std::shared_ptr pFrame = std::make_shared<decoder_sdk::Frame>();
            if (pDecoder_ && pDecoder_->controller_.audioQueue().tryPop(*pFrame) &&
                pFrame->isValid()) {
                emit pDecoder_->videoFrameReady(pFrame);
                shouldSleep = false;
            }
        }

        {
            // 视频，需要验证视频帧有效，且（是关键帧 或 decodeKeyFrame_为true）
            std::shared_ptr pFrame = std::make_shared<decoder_sdk::Frame>();
            if (pDecoder_ && pDecoder_->controller_.videoQueue().tryPop(*pFrame) &&
                pFrame->isValid()) {
                // AMF硬解，暂不检测关键帧
                if (pFrame->pixelFormat() == decoder_sdk::ImageFormat::kAmf ||
                    (pFrame->keyFrame() == 1 || decodeKeyFrame_)) {
                    emit pDecoder_->videoFrameReady(pFrame);
                    if (!decodeKeyFrame_)
                        decodeKeyFrame_ = true;

                    shouldSleep = false;
                }
            }
        }

        if (shouldSleep) {
            QThread::msleep(1);
        }
    }
}
#pragma endregion

#pragma region SafeDeleteThread
SafeDeleteThread::SafeDeleteThread(StreamDecoder *const decoder, QObject *parent)
    : QThread(parent), pDecoder_{decoder}
{
}

SafeDeleteThread::~SafeDeleteThread()
{
}

void SafeDeleteThread::run()
{
    while ((pDecoder_ && pDecoder_->isOpening_.load()) || !isInterruptionRequested()) {
        QThread::msleep(1);
    }

    if (pDecoder_) {
        pDecoder_->close();
    }
}
#pragma endregion

#pragma region StreamDecoder
StreamDecoder::StreamDecoder(QObject *parent)
    : QObject(parent), controller_{decoder_sdk::DecoderController()}
{
    controller_.addGlobalEventListener(std::bind(&StreamDecoder::streamEventCallback, this,
                                                 std::placeholders::_1, std::placeholders::_2));
    controller_.setLoopMode(decoder_sdk::LoopMode::kInfinite);

    decodeThread_ = new DecoderThread(this);
    safeDeleteThread_ = new SafeDeleteThread(this);
}

StreamDecoder::~StreamDecoder()
{
    if (decodeThread_) {
        if (decodeThread_->isRunning()) {
            decodeThread_->requestInterruption();
            decodeThread_->quit();
            if (!decodeThread_->wait(3000)) {
                // decodeThread中没有需要注意的共享资源，可以直接terminate
                decodeThread_->terminate();
                decodeThread_->wait();
            }
        }

        decodeThread_->deleteLater();
        decodeThread_ = nullptr;
    }

    if (safeDeleteThread_) {
        if (safeDeleteThread_->isRunning()) {
            safeDeleteThread_->requestInterruption();
            safeDeleteThread_->quit();
            if (!safeDeleteThread_->wait(3000)) {
                // safeDeleteThread_中没有需要注意的共享资源，可以直接terminate
                safeDeleteThread_->terminate();
                safeDeleteThread_->wait();
            }
        }

        safeDeleteThread_->deleteLater();
        safeDeleteThread_ = nullptr;
    }
}

void StreamDecoder::safeDelete()
{
    if (!isOpening_.load()) {
        close();
    } else {
        if (safeDeleteThread_)
            safeDeleteThread_->start();
    }
}

void StreamDecoder::doTask(Task task)
{
    QMutexLocker l(&mutex_);

    switch (task) {
        case Task::kPause:
            pause();
            break;
        case Task::kResume:
            resume();
            break;
        case Task::kClose:
            safeDelete();
            break;
        default:
            break;
    }
}

void StreamDecoder::startRecoding(const QString &recordPath)
{
    if (controller_.isRecording())
        return;

    controller_.startRecording(recordPath.toLocal8Bit().toStdString());
}

void StreamDecoder::stopRecording()
{
    if (!controller_.isRecording())
        return;

    controller_.stopRecording();
}

void StreamDecoder::seek(double pos)
{
    controller_.seek(pos);
}

void StreamDecoder::setSpeed(double speed)
{
    controller_.setSpeed(speed);
}

void StreamDecoder::setLoopMode(decoder_sdk::LoopMode mode, int maxLoops)
{
    controller_.setLoopMode(mode, maxLoops);
}

void StreamDecoder::resetLoopCount()
{
    controller_.resetLoopCount();
}

bool StreamDecoder::close()
{
    if (controller_.isRecording()) {
        controller_.stopRecording();
    }

    if (!controller_.isDecodeStopped()) {
        controller_.stopDecode();
    }

    if (decodeThread_) {
        decodeThread_->requestInterruption();
        decodeThread_->quit();
        if (!decodeThread_->wait(3000)) {
            qDebug() << __FUNCTION__ << "decoder terminated !!!";
            // decodeThread中没有需要注意的共享资源，可以直接terminate
            decodeThread_->terminate();
            decodeThread_->wait();
        }
    }
    const auto ret = controller_.close();
    if (ret) {
        emit requestToDelete();
    }

#if DEBUG_DECODER
    g_decodingDecoder.fetch_sub(1);
    qDebug()
        << QStringLiteral("****** decoding decoder count: %1 ******").arg(g_decodingDecoder.load());
#endif

    return ret;
}

void StreamDecoder::openAsync(const QString &url, const decoder_sdk::DecoderConfig &config)
{
    controller_.openAsync(url.toStdString(), config,
                          std::bind(&StreamDecoder::openCallback, this, std::placeholders::_1,
                                    std::placeholders::_2, std::placeholders::_3));
}

bool StreamDecoder::pause()
{
    bool ret = controller_.pause();
    if (controller_.isRealTimeUrl()) {
        ret = controller_.stopDecode();
    }

    if (decodeThread_) {
        decodeThread_->requestInterruption();
        decodeThread_->quit();
        decodeThread_->wait();
    }

#if DEBUG_DECODER
    g_decodingDecoder.fetch_sub(1);
    qDebug()
        << QStringLiteral("****** decoding decoder count: %1 ******").arg(g_decodingDecoder.load());
#endif

    return ret;
}

bool StreamDecoder::resume()
{
    bool ret = false;
    if (controller_.isRealTimeUrl()) {
        ret = controller_.startDecode();
    }
    ret = controller_.resume();

#if DEBUG_DECODER
    g_decodingDecoder.fetch_add(1);
    qDebug()
        << QStringLiteral("****** decoding decoder count: %1 ******").arg(g_decodingDecoder.load());
#endif

    return ret;
}

void StreamDecoder::openCallback(decoder_sdk::AsyncOpenResult result, bool openSuccess,
                                 const std::string &errorMessage)
{
    emit openResultReady(result == decoder_sdk::AsyncOpenResult::kSuccess,
                         QString::fromStdString(errorMessage));

    if (result == decoder_sdk::AsyncOpenResult::kSuccess) {
        controller_.startDecode();

#if DEBUG_DECODER
        g_decodingDecoder.fetch_add(1);
        qDebug() << QStringLiteral("****** decoding decoder count: %1 ******")
                        .arg(g_decodingDecoder.load());
#endif
    }
}

void StreamDecoder::decode()
{
    if (decodeThread_ && !decodeThread_->isRunning())
        decodeThread_->start();
}

void StreamDecoder::streamEventCallback(decoder_sdk::EventType type,
                                        std::shared_ptr<decoder_sdk::EventArgs> event)
{
    switch (type) {
        case decoder_sdk::EventType::kDecodeFirstFrame:
            decode();
            break;
        case decoder_sdk::EventType::kStreamClosed: {
            std::shared_ptr<decoder_sdk::Frame> nullFrame = std::make_shared<decoder_sdk::Frame>();
            emit videoFrameReady(nullFrame);
            break;
        }
        default:
            break;
    }

    isOpening_.store(type == decoder_sdk::EventType::kStreamOpening);

    if (type != decoder_sdk::EventType::kStreamOpening && safeDeleteThread_ &&
        safeDeleteThread_->isRunning()) {
        safeDeleteThread_->requestInterruption();
    }

    emit eventUpdated(QString::fromStdString(controller_.url()), type, event);
}

#pragma endregion

#pragma region StreamDecoderWorker
StreamDecoderWorker::StreamDecoderWorker(const QString &key, QObject *parent /*= nullptr*/)
    : QThread(parent), key_{key}
{
    decoder_ = new StreamDecoder;
    thread_ = new QThread;
    thread_->setObjectName(key);
    decoder_->moveToThread(thread_);
    thread_->start();

    // ============== Worker => Decoder的信号 ============== //
    connect(this, &StreamDecoderWorker::requestToStartRecoding, decoder_,
            &StreamDecoder::startRecoding);
    connect(this, &StreamDecoderWorker::requestToStopRecording, decoder_,
            &StreamDecoder::stopRecording);
    connect(this, &StreamDecoderWorker::requestToSeek, decoder_, &StreamDecoder::seek);
    connect(this, &StreamDecoderWorker::requestToSetSpeed, decoder_, &StreamDecoder::setSpeed);
    connect(this, &StreamDecoderWorker::requestToSetLoopMode, decoder_,
            &StreamDecoder::setLoopMode);
    connect(this, &StreamDecoderWorker::requestToResetLoopCount, decoder_,
            &StreamDecoder::resetLoopCount);
    connect(this, &StreamDecoderWorker::requestToOpenAsync, decoder_, &StreamDecoder::openAsync);
    connect(this, &StreamDecoderWorker::requestToDoTask, decoder_, &StreamDecoder::doTask,
            Qt::BlockingQueuedConnection); // 阻塞当前线程
    // ===================================================== //

    // ============== Decoder => Worker的信号 ============== //
    connect(decoder_, &StreamDecoder::openResultReady, this,
            &StreamDecoderWorker::onOpenResultReady);
    connect(decoder_, &StreamDecoder::requestToDelete, this,
            &StreamDecoderWorker::onDecoderRequestToDelete);

    connect(decoder_, &StreamDecoder::videoFrameReady, this, &StreamDecoderWorker::videoFrameReady);
    connect(decoder_, &StreamDecoder::eventUpdated, this, &StreamDecoderWorker::onEventUpdated);
    // ===================================================== //

    // ================== 线程的销毁信号 =================== //
    connect(thread_, &QThread::finished, thread_, &QThread::deleteLater);
    connect(thread_, &QThread::finished, this, &QThread::deleteLater);
    // ===================================================== //

    // ========== 程序即将退出时，不再往外送帧 ============= //
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        disconnect(decoder_, &StreamDecoder::videoFrameReady, this,
                   &StreamDecoderWorker::videoFrameReady);
    });
    // ===================================================== //

#if DEBUG_DECODER
    g_existingDecoder.fetch_add(1);
    qDebug()
        << QStringLiteral("****** existing decoder count: %1 ******").arg(g_existingDecoder.load());
#endif
}

StreamDecoderWorker::~StreamDecoderWorker()
{
    if (isRunning()) {
        requestInterruption();
        condition_.notify_all(); // 防止run循环中还有休眠的条件变量，这里唤醒全部
        quit();

        wait();
    }

#if DEBUG_DECODER
    g_existingDecoder.fetch_sub(1);
    qDebug()
        << QStringLiteral("****** existing decoder count: %1 ******").arg(g_existingDecoder.load());
#endif

    if (decoder_) {
        delete decoder_;
        decoder_ = nullptr;
    }
}

void StreamDecoderWorker::appendTask(StreamDecoder::Task task)
{
    std::lock_guard l(mutex_);

    // 判断任务是否需要执行，不需要执行，则直接返回
    if (!shouldExecuteTask(task))
        return;

    // 获得队列中的第一个任务，如果是关闭，则此时不应该再接受其它任务
    if (!tasks_.isEmpty() && tasks_.front() == StreamDecoder::Task::kClose) {
        // 唤醒消费者
        condition_.notify_one();
        // 返回
        return;
    }

    // 清空之前的任务，只保留最新的
    tasks_.clear();
    tasks_.append(task);

    // 如果当前的任务是关闭，则通知外部
    if (task == StreamDecoder::Task::kClose) {
        decoderPreparingToClose_ = true;
    }

    // 唤醒消费者
    condition_.notify_one();
}

void StreamDecoderWorker::open(const QString &url, const decoder_sdk::DecoderConfig &config)
{
    // 只开启一次，后续加进来的player，会调用resume，防止当前解码器正在暂停
    if (!once_.load()) {
        once_.store(true);
        emit requestToOpenAsync(url, config);
    } else {
        appendTask(StreamDecoder::Task::kResume);

        // 广播流信息和解码器信息，通知后续加入的player
        emit requestToBroadcastStreamAndDecoderInfo();
    }
}

bool StreamDecoderWorker::isRecodering() const
{
    return isRecording_;
}

bool StreamDecoderWorker::decoderPreparingToClose() const
{
    return decoderPreparingToClose_;
}

QString StreamDecoderWorker::key() const
{
    return key_;
}

void StreamDecoderWorker::registerPlayer(VideoPlayerImpl *player)
{
    if (!player || refPlayers_.contains(player) || decoder_.isNull())
        return;

    connect(this, &StreamDecoderWorker::videoFrameReady, player, &VideoPlayerImpl::videoFrameReady,
            Qt::UniqueConnection);
    connect(this, &StreamDecoderWorker::eventUpdated, player,
            &VideoPlayerImpl::onDecoderEventChanged, Qt::UniqueConnection);
    refPlayers_ << player;
}

void StreamDecoderWorker::unRegisterPlayer(VideoPlayerImpl *player)
{
    if (!player || !refPlayers_.contains(player) || decoder_.isNull())
        return;

    disconnect(this, &StreamDecoderWorker::videoFrameReady, player,
               &VideoPlayerImpl::videoFrameReady);
    disconnect(this, &StreamDecoderWorker::eventUpdated, player,
               &VideoPlayerImpl::onDecoderEventChanged);
    refPlayers_.removeOne(player);
}

void StreamDecoderWorker::run()
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

void StreamDecoderWorker::setRecordingStatus(bool status)
{
    isRecording_ = status;
}

void StreamDecoderWorker::onOpenResultReady(bool res, const QString &errorMsg)
{
    // 这里不管成功还是失败，都开启事件循环，不能影响后续任务的接收
    // 开启事件循环
    start();
}

void StreamDecoderWorker::onDecoderRequestToDelete()
{
    decoder_->deleteLater();

    if (thread_ && thread_->isRunning()) {
        thread_->requestInterruption();
        thread_->quit();
        thread_->wait();
    }

    emit aboutToDelete(key_);
}

void StreamDecoderWorker::onEventUpdated(const QString &url, decoder_sdk::EventType type,
                                         const std::shared_ptr<decoder_sdk::EventArgs> &event)
{
    switch (type) {
        case decoder_sdk::EventType::kRecordingStarted:
            // 设置当前的录像状态
            setRecordingStatus(true);
            break;
        case decoder_sdk::EventType::kRecordingStopped:
            // 设置当前的录像状态
            setRecordingStatus(false);
            break;
        case decoder_sdk::EventType::kRecordingError:
            if (refPlayers_.isEmpty()) {
                emit requestToStopRecording();
            }
            break;
        default:
            break;
    }

    emit eventUpdated(url, type, event);
}

bool StreamDecoderWorker::shouldExecuteTask(StreamDecoder::Task task) const
{
    // 如果当前关联的播放器为空，则总是可以执行任务的
    if (refPlayers_.isEmpty())
        return true;

    switch (task) {
        case StreamDecoder::Task::kPause:
            // 如果有正在播放、或即将播放的关联播放器，则不应该处理暂停命令
            for (auto *const player : refPlayers_) {
                if (player->playerState() == Stream::PlayerState::Playing ||
                    player->playerState() == Stream::PlayerState::Resume) {
                    return false;
                }
            }

            // 所有播放器都没有播放时，才可以暂停
            return true;
        case StreamDecoder::Task::kResume:
            // 恢复播放总是可以执行的
            return true;
        case StreamDecoder::Task::kClose:
            // 只有当前没有任何关联播放器时，才能关闭
            return refPlayers_.isEmpty();
        default:
            break;
    }

    return false;
}

#pragma endregion
