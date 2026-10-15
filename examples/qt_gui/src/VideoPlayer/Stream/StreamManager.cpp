#include "StreamManager.h"
#include "CommonUtils.h"
#include "InternalUtils.h"
#include "Stream.h"
#include "VideoPlayerImpl.h"

#include <QApplication>
#include <QDebug>
#include <QList>
#include <QMutexLocker>
#include <QThread>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>

namespace {
    // Retiring pipeline数量超过此值时触发告警和强制回收检查。
    constexpr int kRetiringPipelineForceDeleteThreshold = 3;
} // namespace

StreamManager *StreamManager::instance()
{
    static StreamManager smm;
    return &smm;
}

QString StreamManager::openStream(VideoPlayerImpl *player, QString url,
                                  Stream::OpenMode openMode,
                                  const std::optional<decoder_sdk::DecoderConfig> &customConfig)
{
    if (!player)
        return {};

    // 获得或创建一个解码器，并获得它的key
    QString key;
    StreamPipeline *worker = getOrCreatePipeline(url, openMode, key);

    // 保存player和decoder之间的映射
    const QString playerId = player->id();
    mapPipelineByPlayer_.insert(playerId, worker);

    // player 未显式 close 就析构时，由稳定uuid统一解绑并清理映射。
    connect(player, &VideoPlayerImpl::aboutToDestroyed, this,
            &StreamManager::onPlayerAboutToDestroyed, Qt::UniqueConnection);

    // 向decoder中绑定player
    worker->registerPlayer(player);

    // 开启解码
    const auto &config = customConfig.has_value() ? customConfig.value() : defaultDecoderConfig();
    if (!worker->ensureOpen(url, config)) {
        worker->unRegisterPlayer(player);
        disconnect(worker, nullptr, player, nullptr);
        mapPipelineByPlayer_.remove(playerId);
        return {};
    }

    return key;
}

StreamPipeline *StreamManager::pipelineByPlayer(VideoPlayerImpl *player)
{
    return player ? mapPipelineByPlayer_.value(player->id(), nullptr) : nullptr;
}

bool StreamManager::closeStream(VideoPlayerImpl *player)
{
    if (!player)
        return false;

    // 获得player对应的decoder
    StreamPipeline *const worker = pipelineByPlayer(player);
    if (!worker)
        return false; // 没有对应的解码器，返回false

    const QString playerId = player->id();

    // 解绑player和decoder
    worker->unRegisterPlayerById(playerId);

    // 断开所有信号
    disconnect(worker, nullptr, player, nullptr);
    disconnect(player, &VideoPlayerImpl::aboutToDestroyed, this,
               &StreamManager::onPlayerAboutToDestroyed);
    // 清空当前的缓存
    mapPipelineByPlayer_.remove(playerId);

    // 向decoder发送关闭的任务
    if (worker->appendTask(DecodeWorker::Task::kClose)) {
        // 只有kClose真正成为终态任务时才进入Retiring；如果任务被降级为kPause，
        // pipeline仍由其它player使用，不能提前移出Active集合。
        if (worker->isClosing()) {
            retirePipeline(worker);
        }
    }
    return true;
}

bool StreamManager::closeSupplementaryStream(const QString &key, VideoPlayerImpl *player)
{
    // 获得key对应的decoder
    const auto workerIt = mapActivePipelineByKey_.constFind(key);
    StreamPipeline *const worker = workerIt == mapActivePipelineByKey_.constEnd()
                                       ? nullptr
                                       : workerIt->pipeline.data();
    if (!worker)
        return false; // 没有对应的解码器，返回false

    // 如果player有效，尝试解绑
    if (player) {
        // 解绑player和decoder
        worker->unRegisterPlayer(player);

        // 断开所有信号
        disconnect(worker, nullptr, player, nullptr);
    }

    // 向decoder发送关闭的任务
    if (worker->appendTask(DecodeWorker::Task::kClose) && worker->isClosing()) {
        retirePipeline(worker);
    }
    return true;
}

bool StreamManager::pause(VideoPlayerImpl *player)
{
    // 获得player对应的decoder
    StreamPipeline *const worker = pipelineByPlayer(player);
    if (!worker)
        return false; // 没有对应的解码器，返回false

    // 向decoder发送暂停的任务
    worker->appendTask(DecodeWorker::Task::kPause);
    return true;
}

bool StreamManager::resume(VideoPlayerImpl *player)
{
    // 获得player对应的decoder
    StreamPipeline *const worker = pipelineByPlayer(player);
    if (!worker)
        return false; // 没有对应的解码器，返回false

    // 向decoder发送恢复的任务
    worker->appendTask(DecodeWorker::Task::kResume);
    return true;
}

bool StreamManager::startRecoding(VideoPlayerImpl *player, const QString &recorderFilePath)
{
    // 获得player对应的decoder
    StreamPipeline *const worker = pipelineByPlayer(player);
    if (!worker || worker->isRecodering(recorderFilePath))
        return false; // 没有对应的解码器，返回false

    // 开启decoder的录像
    emit worker->requestToStartRecoding(recorderFilePath);
    return true;
}

bool StreamManager::stopRecoding(VideoPlayerImpl *player, const QString &recorderFilePath)
{
    // 获得player对应的decoder
    StreamPipeline *const worker = pipelineByPlayer(player);
    if (!worker || !worker->isRecodering(recorderFilePath))
        return false; // 没有对应的解码器，返回false

    // 停止decoder的录像
    emit worker->requestToStopRecording(recorderFilePath);
    return true;
}

bool StreamManager::isRecoding(VideoPlayerImpl *player, const QString &recorderFilePath)
{
    // 获得player对应的decoder
    StreamPipeline *const worker = pipelineByPlayer(player);
    if (!worker)
        return false;

    return worker->isRecodering(recorderFilePath);
}

bool StreamManager::seek(VideoPlayerImpl *player, double pts)
{
    // 获得player对应的decoder
    StreamPipeline *const worker = pipelineByPlayer(player);
    if (!worker)
        return false;

    emit worker->requestToSeek(pts);
    return true;
}

bool StreamManager::setSpeed(VideoPlayerImpl *player, double speed)
{
    // 获得player对应的decoder
    StreamPipeline *const worker = pipelineByPlayer(player);
    if (!worker)
        return false;

    emit worker->requestToSetSpeed(speed);
    return true;
}

bool StreamManager::setLoopMode(VideoPlayerImpl *player, decoder_sdk::LoopMode mode, int maxLoops)
{
    // 获得player对应的decoder
    StreamPipeline *const worker = pipelineByPlayer(player);
    if (!worker)
        return false;

    emit worker->requestToSetLoopMode(mode, maxLoops);
    return true;
}

bool StreamManager::resetLoopCount(VideoPlayerImpl *player)
{
    // 获得player对应的decoder
    StreamPipeline *const worker = pipelineByPlayer(player);
    if (!worker)
        return false;

    emit worker->requestToResetLoopCount();
    return true;
}

bool StreamManager::switchStream(const QString &key, VideoPlayerImpl *player)
{
    // 如果player非法，返回
    if (!player)
        return false;

    const QString playerId = player->id();

    // 获得key对应的新decoder
    const auto newWorkerIt = mapActivePipelineByKey_.constFind(key);
    StreamPipeline *const newWorker = newWorkerIt == mapActivePipelineByKey_.constEnd()
                                          ? nullptr
                                          : newWorkerIt->pipeline.data();
    if (!newWorker || newWorker->isClosing())
        return false; // 没有可复用的Active解码器，返回false

    // 获得player对应的旧decoder
    StreamPipeline *const oldWorker = pipelineByPlayer(player);
    // 如果当前存在旧decodre，则先解绑
    if (oldWorker) {
        // 先暂停旧的
        oldWorker->appendTask(DecodeWorker::Task::kPause);

        // 解绑player和decoder
        oldWorker->unRegisterPlayerById(playerId);

        // 断开所有信号
        disconnect(oldWorker, nullptr, player, nullptr);
        // 清空当前的缓存
        mapPipelineByPlayer_.remove(playerId);
    }

    // 绑定player和newWorker
    mapPipelineByPlayer_.insert(playerId, newWorker);

    // 向decoder中绑定player
    newWorker->registerPlayer(player);

    // 恢复新的解码
    newWorker->appendTask(DecodeWorker::Task::kResume);
    return true;
}

QString StreamManager::createStream(const QString &url, Stream::OpenMode openMode)
{
    // 获得或创建一个解码器，并获得它的key
    QString key;
    StreamPipeline *worker = getOrCreatePipeline(url, openMode, key);
    if (!worker)
        return {};

    // 开启解码
    const auto config = defaultDecoderConfig();
    if (!worker->ensureOpen(url, config)) {
        return {};
    }

    // 开启后暂停
    worker->appendTask(DecodeWorker::Task::kPause);

    return key;
}

bool StreamManager::isStreamExist(const QString &key) const
{
    const auto it = mapActivePipelineByKey_.constFind(key);
    return it != mapActivePipelineByKey_.constEnd() && !it->pipeline.isNull();
}

const decoder_sdk::DecoderConfig &StreamManager::defaultDecoderConfig() const
{
    return defaultDecoderConfig_;
}

bool StreamManager::registerEventCallback(VideoPlayerImpl *player, decoder_sdk::EventType type,
                                          std::function<void(const QString &url, decoder_sdk::EventType type,
                                                             const std::shared_ptr<decoder_sdk::EventArgs> &event)>
                                              cb)
{
    // 获得player对应的decoder
    StreamPipeline *const worker = pipelineByPlayer(player);
    if (!worker)
        return false;

    return worker->registerEventCallback(type, cb);
}

void StreamManager::onWorkerAboutToDelete(const QString &key, quint64 pipelineId)
{
    const auto activeIt = mapActivePipelineByKey_.find(key);
    if (activeIt != mapActivePipelineByKey_.end() && activeIt->pipelineId == pipelineId) {
        StreamPipeline *const worker = activeIt->pipeline.data();
        mapActivePipelineByKey_.erase(activeIt);
        if (worker) {
            removePlayerBindingsForPipeline(worker);
        }
        return;
    }

    const auto retiringIt = mapRetiringPipelineById_.find(pipelineId);
    if (retiringIt == mapRetiringPipelineById_.end()) {
        // 实例可能已经被强制删除，或这是更早实例的延迟通知。
        return;
    }

    const PipelineRecord record = retiringIt.value();
    mapRetiringPipelineById_.erase(retiringIt);

    const int remaining = (std::max)(0, retiringCountByKey_.value(record.key) - 1);
    if (remaining > 0) {
        retiringCountByKey_.insert(record.key, remaining);
    } else {
        retiringCountByKey_.remove(record.key);
    }
    updateRetiringWarning(record.key);
}

StreamPipeline *StreamManager::createPipeline(const QString &key)
{
    auto *worker = new StreamPipeline(key);
    PipelineRecord record;
    record.key = key;
    record.pipelineId = worker->pipelineId();
    record.pipeline = worker;
    mapActivePipelineByKey_.insert(key, record);

    connect(worker, &StreamPipeline::aboutToDelete,
            this, &StreamManager::onWorkerAboutToDelete);
    return worker;
}

void StreamManager::retirePipeline(StreamPipeline *worker)
{
    if (!worker || !worker->isClosing()) {
        return;
    }

    const QString key = worker->key();
    const auto activeIt = mapActivePipelineByKey_.find(key);
    if (activeIt == mapActivePipelineByKey_.end() || activeIt->pipeline != worker) {
        return;
    }

    const PipelineRecord record = activeIt.value();
    mapActivePipelineByKey_.erase(activeIt);
    mapRetiringPipelineById_.insert(record.pipelineId, record);
    removePlayerBindingsForPipeline(worker);

    const int retiringCount = retiringCountByKey_.value(key) + 1;
    retiringCountByKey_.insert(key, retiringCount);
    updateRetiringWarning(key);

    if (retiringCount > kRetiringPipelineForceDeleteThreshold) {
        // 阈值超限后的唯一强制回收入口。
        //
        // 实践中需要临时关闭强制停止时，只注释下面这一行调用即可；不要注释
        // queueForceDeleteOldestRetiringPipeline的函数定义，否则会产生链接错误。
        //
        // 该函数只把任务排到下一轮事件循环，不会阻塞当前open流程。
        queueForceDeleteOldestRetiringPipeline(key);
    }
}

void StreamManager::removePlayerBindingsForPipeline(StreamPipeline *worker)
{
    if (!worker) {
        return;
    }

    for (auto it = mapPipelineByPlayer_.begin(); it != mapPipelineByPlayer_.end();) {
        if (it.value() == worker) {
            it = mapPipelineByPlayer_.erase(it);
        } else {
            ++it;
        }
    }
}

void StreamManager::updateRetiringWarning(const QString &key)
{
    const int retiringCount = retiringCountByKey_.value(key, 0);
    if (retiringCount <= kRetiringPipelineForceDeleteThreshold) {
        retiringWarningActiveByKey_.remove(key);
        return;
    }

    if (retiringWarningActiveByKey_.value(key, false)) {
        return;
    }

    retiringWarningActiveByKey_.insert(key, true);
    qWarning() << QStringLiteral(
        "[StreamManager] Retiring pipeline threshold exceeded: key=%1, count=%2, threshold=%3")
                      .arg(key)
                      .arg(retiringCount)
                      .arg(kRetiringPipelineForceDeleteThreshold);
}

void StreamManager::queueForceDeleteOldestRetiringPipeline(const QString &key)
{
    // 通过Qt事件队列延迟执行，保证调用方所在的open/close流程先完整返回。
    // 不使用lambda，实际删除逻辑集中在可单独调试的成员函数中。
    const bool queued = QMetaObject::invokeMethod(
        this,
        "forceDeleteOldestRetiringPipeline",
        Qt::QueuedConnection,
        Q_ARG(QString, key));
    if (!queued) {
        qWarning() << QStringLiteral(
            "[StreamManager] Failed to queue retiring pipeline cleanup: key=%1").arg(key);
    }
}

void StreamManager::forceDeleteOldestRetiringPipeline(const QString &key)
{
    if (retiringCountByKey_.value(key, 0) <= kRetiringPipelineForceDeleteThreshold) {
        return;
    }

    // QMap按pipelineId升序保存Retiring记录，因此第一个匹配key的实例就是最早实例。
    auto oldestIt = mapRetiringPipelineById_.begin();
    while (oldestIt != mapRetiringPipelineById_.end() && oldestIt->key != key) {
        ++oldestIt;
    }
    if (oldestIt == mapRetiringPipelineById_.end()) {
        return;
    }

    const PipelineRecord record = oldestIt.value();
    mapRetiringPipelineById_.erase(oldestIt);

    const int remaining = (std::max)(0, retiringCountByKey_.value(record.key) - 1);
    if (remaining > 0) {
        retiringCountByKey_.insert(record.key, remaining);
    } else {
        retiringCountByKey_.remove(record.key);
    }
    updateRetiringWarning(record.key);

    StreamPipeline *const worker = record.pipeline.data();
    if (worker) {
        // 强制回收跳过正常kClose信号链，因此先请求线程停止并等待线程退出，
        // 最后才删除对象。DecoderSDK的析构函数负责完成底层资源收尾。
        qWarning() << QStringLiteral(
            "[StreamManager] Force deleting retiring pipeline: key=%1, pipelineId=%2")
                          .arg(record.key)
                          .arg(record.pipelineId);
        worker->requestStop();
        worker->wait();
        delete worker;
    }

    // 仍超阈值时在下一轮继续回收一个实例，避免单个事件循环中连续同步销毁
    // 多个pipeline造成长时间阻塞。
    if (retiringCountByKey_.value(key, 0) > kRetiringPipelineForceDeleteThreshold) {
        queueForceDeleteOldestRetiringPipeline(key);
    }
}

void StreamManager::onPlayerAboutToDestroyed(const QString &uuid)
{
    QList<StreamPipeline *> workersToRetire;

    // 统一解绑所有仍持有该player的Active管线，再清理主映射，避免裸指针残留。
    for (auto it = mapActivePipelineByKey_.begin(); it != mapActivePipelineByKey_.end(); ++it) {
        StreamPipeline *const worker = it->pipeline.data();
        if (worker && worker->hasPlayer(uuid)) {
            worker->unRegisterPlayerById(uuid);
            if (worker->appendTask(DecodeWorker::Task::kClose) && worker->isClosing()) {
                workersToRetire.append(worker);
            }
        }
    }

    for (StreamPipeline *worker : workersToRetire) {
        retirePipeline(worker);
    }

    mapPipelineByPlayer_.remove(uuid);
}

void StreamManager::onAppAboutToQuit()
{
    clearAllStreams();
}

StreamPipeline *StreamManager::getOrCreatePipeline(const QString &url,
                                                       Stream::OpenMode openMode, QString &key)
{
    if (openMode == Stream::OpenMode::kExclusive) {
        // 独占方式打开时，key是唯一的，这里用uuid代替
        key = QUuid::createUuid().toString(QUuid::WithoutBraces);
        return createPipeline(key);
    }

    // 复用方式打开的key就是url。活动实例仍可复用；已经进入终态的实例先移入
    // Retiring，然后立即创建一个新Active实例，不等待旧实例关闭。
    key = url;
    const auto activeIt = mapActivePipelineByKey_.find(key);
    if (activeIt != mapActivePipelineByKey_.end()) {
        StreamPipeline *const worker = activeIt->pipeline.data();
        if (worker && !worker->isClosing()) {
            return worker;
        }
        if (worker) {
            retirePipeline(worker);
        }
    }

    return createPipeline(key);
}

void StreamManager::initDefaultDecoderConfig()
{
    defaultDecoderConfig_.hwAccelType = decoder_sdk::HWAccelType::kAuto; 
    defaultDecoderConfig_.backendHwAccelType = decoder_sdk::HWAccelType::kD3d11va;
    defaultDecoderConfig_.swVideoOutFormat = decoder_sdk::ImageFormat::kYUV420P;
    defaultDecoderConfig_.requireFrameInSystemMemory = false;
    defaultDecoderConfig_.decodeMediaTypes = decoder_sdk::MediaType::kAll; 
    defaultDecoderConfig_.enableHardwareFallback = true;
    defaultDecoderConfig_.enableAutoReconnect = true;
    defaultDecoderConfig_.maxReconnectAttempts = -1;
    defaultDecoderConfig_.reconnectIntervalMs = 3000;
    defaultDecoderConfig_.preBufferConfig.enablePreBuffer = false;
    defaultDecoderConfig_.audioInterleaved = true;
    defaultDecoderConfig_.enableJitterDetector = false;

    defaultDecoderConfig_.createHwContextCallback =
        std::bind(&utils::createHwContextCallback, std::placeholders::_1);
    defaultDecoderConfig_.freeHwContextCallback =
        std::bind(&utils::freeHwContextCallback, std::placeholders::_1, std::placeholders::_2);
}

void StreamManager::clearAllStreams()
{
    QList<StreamPipeline *> workers;
    for (const PipelineRecord &record : mapActivePipelineByKey_) {
        StreamPipeline *const worker = record.pipeline.data();
        if (worker && !workers.contains(worker)) {
            workers.append(worker);
        }
    }
    for (const PipelineRecord &record : mapRetiringPipelineById_) {
        StreamPipeline *const worker = record.pipeline.data();
        if (worker && !workers.contains(worker)) {
            workers.append(worker);
        }
    }

    // 先统一请求停止，再等待退出，避免逐个等待拉长退出时间。
    for (StreamPipeline *worker : workers) {
        worker->requestStop();
    }
    for (StreamPipeline *worker : workers) {
        worker->wait();
    }
    for (StreamPipeline *worker : workers) {
        delete worker;
    }

    mapActivePipelineByKey_.clear();
    mapRetiringPipelineById_.clear();
    retiringCountByKey_.clear();
    retiringWarningActiveByKey_.clear();
    mapPipelineByPlayer_.clear();
}

StreamManager::StreamManager(QObject *parent /*= nullptr*/)
    : QObject(parent)
{
    initDefaultDecoderConfig();

    connect(qApp, &QApplication::aboutToQuit, this, &StreamManager::onAppAboutToQuit);
}

StreamManager::~StreamManager()
{
    clearAllStreams();
}
