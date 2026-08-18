#include "StreamManager.h"
#include "CommonUtils.h"
#include "InternalUtils.h"
#include "Stream.h"
#include "VideoPlayerImpl.h"

#include <QApplication>
#include <QMutexLocker>
#include <QThread>
#include <QtConcurrent/QtConcurrent>

StreamManager *StreamManager::instance()
{
    static StreamManager smm;
    return &smm;
}

QString StreamManager::openStream(VideoPlayerImpl *player, QString url,
                                  Stream::OpenMode openMode,
                                  const std::optional<decoder_sdk::DecoderConfig> &customConfig)
{
    // 获得或创建一个解码器，并获得它的key
    QString key;
    StreamPipeline *worker = getOrCreatePipeline(url, openMode, key);

    // 保存player和decoder之间的映射
    mapPipelineByPlayer_.insert(player, worker);

    // 向decoder中绑定player
    worker->registerPlayer(player);

    // 开启解码
    const auto &config = customConfig.has_value() ? customConfig.value() : defaultDecoderConfig();
    worker->open(url, config);

    return key;
}

StreamPipeline *StreamManager::pipelineByPlayer(VideoPlayerImpl *player)
{
    return mapPipelineByPlayer_.value(player, nullptr);
}

bool StreamManager::closeStream(VideoPlayerImpl *player)
{
    // 获得player对应的decoder
    StreamPipeline *const worker = pipelineByPlayer(player);
    if (!worker)
        return false; // 没有对应的解码器，返回false

    // 解绑player和decoder
    worker->unRegisterPlayer(player);

    // 断开所有信号
    disconnect(worker, nullptr, player, nullptr);
    // 清空当前的缓存
    mapPipelineByPlayer_.remove(player);

    // 向decoder发送关闭的任务
    worker->appendTask(DecodeWorker::Task::kClose);
    return true;
}

bool StreamManager::closeSupplementaryStream(const QString &key, VideoPlayerImpl *player)
{
    // 获得key对应的decoder
    StreamPipeline *const worker = mapPipelineByKey_.value(key);
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
    worker->appendTask(DecodeWorker::Task::kClose);
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

bool StreamManager::startRecoding(VideoPlayerImpl *player, const QString &recodDir)
{
    // 获得player对应的decoder
    StreamPipeline *const worker = pipelineByPlayer(player);
    if (!worker || worker->isRecodering())
        return false; // 没有对应的解码器，返回false

    // 开启decoder的录像
    emit worker->requestToStartRecoding(recodDir);
    return true;
}

bool StreamManager::stopRecoding(VideoPlayerImpl *player)
{
    // 获得player对应的decoder
    StreamPipeline *const worker = pipelineByPlayer(player);
    if (!worker || !worker->isRecodering())
        return false; // 没有对应的解码器，返回false

    // 停止decoder的录像
    emit worker->requestToStopRecording();
    return true;
}

bool StreamManager::isRecoding(VideoPlayerImpl *player)
{
    // 获得player对应的decoder
    StreamPipeline *const worker = pipelineByPlayer(player);
    if (!worker)
        return false;

    return worker->isRecodering();
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

    // 获得key对应的新decoder
    StreamPipeline *const newWorker = mapPipelineByKey_.value(key);
    if (!newWorker)
        return false; // 没有对应的解码器，返回false

    // 获得player对应的旧decoder
    StreamPipeline *const oldWorker = pipelineByPlayer(player);
    // 如果当前存在旧decodre，则先解绑
    if (oldWorker) {
        // 先暂停旧的
        oldWorker->appendTask(DecodeWorker::Task::kPause);

        // 解绑player和decoder
        oldWorker->unRegisterPlayer(player);

        // 断开所有信号
        disconnect(oldWorker, nullptr, player, nullptr);
        // 清空当前的缓存
        mapPipelineByPlayer_.remove(player);
    }

    // 绑定player和newWorker
    mapPipelineByPlayer_.insert(player, newWorker);

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
    worker->open(url, config);

    // 开启后暂停
    worker->appendTask(DecodeWorker::Task::kPause);

    return key;
}

bool StreamManager::isStreamExist(const QString &key) const
{
    return mapPipelineByKey_.contains(key) && mapPipelineByKey_[key];
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

void StreamManager::onWorkerAboutToDelete(const QString &key)
{
    mapPipelineByKey_.remove(key);
}

void StreamManager::onAppAboutToQuit()
{
    clearAllStreams();
}

StreamPipeline *StreamManager::getOrCreatePipeline(const QString &url,
                                                       Stream::OpenMode openMode, QString &key)
{
    StreamPipeline *worker = nullptr;
    if (openMode == Stream::OpenMode::kExclusive) {
        // 独占方式打开时，key是唯一的，这里用uuid代替
        key = QUuid::createUuid().toString(QUuid::WithoutBraces);

        worker = new StreamPipeline(key);
        mapPipelineByKey_.insert(key, worker);
        // 连接decoder和manager之间的信号
        connect(worker, &StreamPipeline::aboutToDelete, this,
                &StreamManager::onWorkerAboutToDelete);
    } else {
        // 复用方式打开是，key是url，先查询当前缓存中有没有，没有的话就新建
        key = url;

        worker = mapPipelineByKey_.value(key, nullptr);
        if (!worker) {
            worker = new StreamPipeline(key);
            mapPipelineByKey_.insert(key, worker);
            // 连接decoder和manager之间的信号
            connect(worker, &StreamPipeline::aboutToDelete, this,
                    &StreamManager::onWorkerAboutToDelete);
        } else {
            // 如果存在worker，则判断worker是不是将亡状态，如果是将亡状态，则应该先把旧的踢出去，重建新的
            // 理论上不会出现这种状态，worker只会是有效指针或是nullptr
            if (worker->decoderPreparingToClose()) {
                qWarning() << QStringLiteral(
                    "******** The StreamPipeline will be deleted and a new one will be "
                    "created for use! ********");
                disconnect(worker, &StreamPipeline::aboutToDelete, this,
                           &StreamManager::onWorkerAboutToDelete);

                worker = new StreamPipeline(key);
                mapPipelineByKey_.insert(key, worker);
                // 连接decoder和manager之间的信号
                connect(worker, &StreamPipeline::aboutToDelete, this,
                        &StreamManager::onWorkerAboutToDelete);
            }
        }
    }

    return worker;
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
    // 清理所有的解码器
    for (auto it = mapPipelineByKey_.begin(); it != mapPipelineByKey_.end(); ++it) {
        StreamPipeline *worker = it.value();
        if (worker) {
            delete worker;
            worker = nullptr;
        }
    }
    mapPipelineByKey_.clear();
    // 清理所有的播放器映射
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
