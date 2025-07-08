#include "StreamManager.h"
#include "Commonutils.h"
#include "StreamDecodeWorker.h"
#include "VideoPlayerImpl.h"

#include <QDebug>
#include <QMutexLocker>
#include <QThread>
#include <QtConcurrent/QtConcurrent>

StreamManager *StreamManager::instance()
{
    static StreamManager smm;
    return &smm;
}

QString StreamManager::openStream(VideoPlayerImpl *player, QString url, QString devId,
                                  Stream::OpenMode openMode)
{
    // 获得或创建一个解码器，并获得它的key
    QString key;
    StreamDecoderWorker *worker = getOrCreateDecoder(url, openMode, key);

    // 保存player和decoder之间的映射
    mapDecoderByWidget_.insert(player, worker);

    // 向decoder中绑定player
    worker->registerPlayer(player);

    // 开启解码
    decoder_sdk::Config config;
    config.hwAccelType = decoder_sdk::HWAccelType::kCuda;
    config.swVideoOutFormat = decoder_sdk::ImageFormat::kNV12;
    config.decodeMediaType = decoder_sdk::Config::DecodeMediaType::kVideo;
    config.enableFrameRateControl = true;
    config.createHwContextCallback =
        std::bind(&StreamManager::createHwContextCallback, this, std::placeholders::_1);
    worker->open(url, config);

    return key;
}

StreamDecoderWorker *StreamManager::streamDecoderByPlayer(VideoPlayerImpl *player)
{
    return mapDecoderByWidget_.value(player, nullptr);
}

bool StreamManager::closeStream(VideoPlayerImpl *player)
{
    // 获得player对应的decoder
    StreamDecoderWorker *const worker = streamDecoderByPlayer(player);
    if (!worker)
        return false; // 没有对应的解码器，返回false

    // 解绑player和decoder
    worker->unRegisterPlayer(player);

    // 断开所有信号
    disconnect(worker, nullptr, player, nullptr);
    // 清空当前的缓存
    mapDecoderByWidget_.remove(player);

    // 向decoder发送关闭的任务
    worker->appendTask(StreamDecoder::Task::kClose);
    return true;
}

bool StreamManager::closeSupplementaryStream(const QString &key, VideoPlayerImpl *player)
{
    // 获得key对应的decoder
    StreamDecoderWorker *const worker = mapDecoderByKey_.value(key);
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
    worker->appendTask(StreamDecoder::Task::kClose);
    return true;
}

bool StreamManager::pause(VideoPlayerImpl *player)
{
    // 获得player对应的decoder
    StreamDecoderWorker *const worker = streamDecoderByPlayer(player);
    if (!worker)
        return false; // 没有对应的解码器，返回false

    // 向decoder发送暂停的任务
    worker->appendTask(StreamDecoder::Task::kPause);
    return true;
}

bool StreamManager::resume(VideoPlayerImpl *player)
{
    // 获得player对应的decoder
    StreamDecoderWorker *const worker = streamDecoderByPlayer(player);
    if (!worker)
        return false; // 没有对应的解码器，返回false

    // 向decoder发送恢复的任务
    worker->appendTask(StreamDecoder::Task::kResume);
    return true;
}

bool StreamManager::startRecoding(VideoPlayerImpl *player, const QString &recodDir)
{
    // 获得player对应的decoder
    StreamDecoderWorker *const worker = streamDecoderByPlayer(player);
    if (!worker || worker->isRecodering())
        return false; // 没有对应的解码器，返回false

    // 开启decoder的录像
    worker->needToStartRecoding(recodDir);
    return true;
}

bool StreamManager::stopRecoding(VideoPlayerImpl *player)
{
    // 获得player对应的decoder
    StreamDecoderWorker *const worker = streamDecoderByPlayer(player);
    if (!worker || !worker->isRecodering())
        return false; // 没有对应的解码器，返回false

    // 停止decoder的录像
    worker->needToStopRecording();
    return true;
}

bool StreamManager::isRecoding(VideoPlayerImpl *player)
{
    // 获得player对应的decoder
    StreamDecoderWorker *const worker = streamDecoderByPlayer(player);
    if (!worker)
        return false;

    return worker->isRecodering();
}

QString StreamManager::defaultRecordFileName(VideoPlayerImpl *player)
{
    // 获得player对应的decoder
    StreamDecoderWorker *const worker = streamDecoderByPlayer(player);
    if (!worker)
        return {};

    return worker->key();
}

bool StreamManager::switchStream(const QString &key, VideoPlayerImpl *player)
{
    // 如果player非法，返回
    if (!player)
        return false;

    // 获得key对应的新decoder
    StreamDecoderWorker *const newWorker = mapDecoderByKey_.value(key);
    if (!newWorker)
        return false; // 没有对应的解码器，返回false

    // 获得player对应的旧decoder
    StreamDecoderWorker *const oldWorker = streamDecoderByPlayer(player);
    // 如果当前存在旧decodre，则先解绑
    if (oldWorker) {
        // 先暂停旧的
        oldWorker->appendTask(StreamDecoder::Task::kPause);

        // 解绑player和decoder
        oldWorker->unRegisterPlayer(player);

        // 断开所有信号
        disconnect(oldWorker, nullptr, player, nullptr);
        // 清空当前的缓存
        mapDecoderByWidget_.remove(player);
    }

    // 绑定player和newWorker
    mapDecoderByWidget_.insert(player, newWorker);

    // 向decoder中绑定player
    newWorker->registerPlayer(player);

    // 恢复新的解码
    newWorker->appendTask(StreamDecoder::Task::kResume);
    return true;
}

QString StreamManager::createStream(const QString &url, Stream::OpenMode openMode)
{
    // 获得或创建一个解码器，并获得它的key
    QString key;
    StreamDecoderWorker *worker = getOrCreateDecoder(url, openMode, key);
    if (!worker)
        return {};

    // 开启解码
    decoder_sdk::Config config;
    config.hwAccelType = decoder_sdk::HWAccelType::kD3d11va;
    config.decodeMediaType = decoder_sdk::Config::DecodeMediaType::kVideo;
    config.enableFrameRateControl = true;
    config.createHwContextCallback =
        std::bind(&StreamManager::createHwContextCallback, this, std::placeholders::_1);
    worker->open(url, config);

    // 开启后暂停
    worker->appendTask(StreamDecoder::Task::kPause);

    return key;
}

bool StreamManager::isStreamExist(const QString &key) const
{
    return mapDecoderByKey_.contains(key) && mapDecoderByKey_[key];
}

void StreamManager::onWorkerAboutToDelete(const QString &key)
{
    mapDecoderByKey_.remove(key);
}

StreamDecoderWorker *StreamManager::getOrCreateDecoder(const QString &url,
                                                       Stream::OpenMode openMode, QString &key)
{
    StreamDecoderWorker *worker = nullptr;
    if (openMode == Stream::OpenMode::kExclusive) {
        // 独占方式打开时，key是唯一的，这里用uuid代替
        key = QUuid::createUuid().toString(QUuid::WithoutBraces);

        worker = new StreamDecoderWorker(key);
        mapDecoderByKey_.insert(key, worker);
        // 连接decoder和manager之间的信号
        connect(worker, &StreamDecoderWorker::aboutToDelete, this,
                &StreamManager::onWorkerAboutToDelete);
    } else {
        // 复用方式打开是，key是url，先查询当前缓存中有没有，没有的话就新建
        key = url;

        worker = mapDecoderByKey_.value(key, nullptr);
        if (!worker) {
            worker = new StreamDecoderWorker(key);
            mapDecoderByKey_.insert(key, worker);
            // 连接decoder和manager之间的信号
            connect(worker, &StreamDecoderWorker::aboutToDelete, this,
                    &StreamManager::onWorkerAboutToDelete);
        } else {
            // 如果存在worker，则判断worker是不是将亡状态，如果是将亡状态，则应该先把旧的踢出去，重建新的
            // 理论上不会出现这种状态，worker只会是有效指针或是nullptr
            if (worker->decoderPreparingToClose()) {
                qWarning() << QStringLiteral(
                    "******** The StreamDecoderWorker will be deleted and a new one will be "
                    "created for use! ********");
                disconnect(worker, &StreamDecoderWorker::aboutToDelete, this,
                           &StreamManager::onWorkerAboutToDelete);

                worker = new StreamDecoderWorker(key);
                mapDecoderByKey_.insert(key, worker);
                // 连接decoder和manager之间的信号
                connect(worker, &StreamDecoderWorker::aboutToDelete, this,
                        &StreamManager::onWorkerAboutToDelete);
            }
        }
    }

    return worker;
}

void *StreamManager::createHwContextCallback(decoder_sdk::HWAccelType type)
{
    switch (type) {
#ifdef D3D11VA_AVAILABLE
        case decoder_sdk::HWAccelType::kD3d11va:
            return D3D11Utils::getD3D11Device().Get();
#endif

#ifdef DXVA2_AVAILABLE
        case decoder_sdk::HWAccelType::kDxva2:
            return DXVA2Utils::getDXVA2DeviceManager().Get();
#endif

        default:
            break;
    }

    return nullptr;
}

StreamManager::StreamManager(QObject *parent /*= nullptr*/) : QObject(parent)
{
}

StreamManager::~StreamManager()
{
}
