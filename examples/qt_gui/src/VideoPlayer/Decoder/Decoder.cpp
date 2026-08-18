#include "Decoder.h"
#include "../Player/VideoPlayerImpl.h"

#include <QThread>

#pragma region Decoder
/*!
 * \class Decoder
 *
 * \brief 标准流解码器的实现类
 *
 * \author ZYX
 * \date 2026/06/29
 */
class Decoder : public QObject {
    Q_OBJECT

public:
    explicit Decoder(QObject *parent = nullptr);
    virtual ~Decoder();

    // 异步的打开流
    void openAsync(const QString &url, const decoder_sdk::DecoderConfig &config);

    // 执行任务
    void doTask(DecodeWorker::Task task);

    // 广播流信息以及解码器信息
    void broadcastStreamAndDecoderInfo();

    // 开启录像，结果根据event变更进行处理
    void startRecoding(const QString &recordPath);
    // 关闭录像，结果根据event变更进行处理
    void stopRecording();
    // 进行跳转
    void seek(double pos);
    // 设置倍速
    void setSpeed(double speed);

    // 设置循环模式
    void setLoopMode(decoder_sdk::LoopMode mode, int maxLoops = -1);
    // 重置循环计数
    void resetLoopCount();

    // 尝试弹出一帧（线程安全）
    bool tryPopFrame(decoder_sdk::MediaType type, decoder_sdk::Frame &frame);

    // 查看队首帧（线程安全，不弹出）
    bool frontFrame(decoder_sdk::MediaType type, decoder_sdk::Frame &frame);

    // 获取队首帧PTS（原子操作，低开销）
    double frontPts(decoder_sdk::MediaType type);

signals:
    // 发送流打开结果
    void openResultReady(bool success, const QString &errorMsg);
    // 发送请求销毁信号，由外部进行销毁
    void requestToDelete();
    // 发送流事件通知
    void eventUpdated(const QString &url, decoder_sdk::EventType type, const std::shared_ptr<decoder_sdk::EventArgs> &event);

    // 发送流信息变更
    void streamInfoUpdated(const std::optional<decoder_sdk::StreamInfo> &info);
    // 发送解码器信息变更
    void decoderInfoUpdated(decoder_sdk::MediaType mediaType, const std::optional<decoder_sdk::DecoderInfo> &info);

private:
    // 安全删除这个Decoder，请用这个关闭StreamDecoder
    void safeDelete();

    // 暂停解码器
    bool pause();
    // 恢复解码器
    bool resume();
    // 关闭解码器
    bool close();

    // 打开流后的回调函数
    void openCallback(decoder_sdk::AsyncOpenResult result, bool openSuccess, const std::string &errorMessage);
    // 流事件的回调函数
    void streamEventCallback(decoder_sdk::EventType type, std::shared_ptr<decoder_sdk::EventArgs> event);

private:
    decoder_sdk::DecoderController controller_;
};

Decoder::Decoder(QObject *parent)
    : QObject(parent)
    , controller_{ decoder_sdk::DecoderController() }
{
    controller_.addGlobalEventListener(std::bind(&Decoder::streamEventCallback, this,
                                                 std::placeholders::_1, std::placeholders::_2));
    controller_.setLoopMode(decoder_sdk::LoopMode::kInfinite);
}

Decoder::~Decoder()
{
}

void Decoder::openAsync(const QString &url, const decoder_sdk::DecoderConfig &config)
{
    controller_.openAsync(url.toStdString(), config,
                          std::bind(&Decoder::openCallback, this, std::placeholders::_1,
                                    std::placeholders::_2, std::placeholders::_3));
}

void Decoder::doTask(DecodeWorker::Task task)
{
    switch (task) {
        case DecodeWorker::Task::kPause:
            pause();
            break;
        case DecodeWorker::Task::kResume:
            resume();
            break;
        case DecodeWorker::Task::kClose:
            safeDelete();
            break;
        default:
            break;
    }
}

void Decoder::broadcastStreamAndDecoderInfo()
{
    emit streamInfoUpdated(controller_.streamInfo());
    emit decoderInfoUpdated(decoder_sdk::MediaType::kVideo, controller_.decoderInfo(decoder_sdk::MediaType::kVideo));
    emit decoderInfoUpdated(decoder_sdk::MediaType::kAudio, controller_.decoderInfo(decoder_sdk::MediaType::kAudio));
}

void Decoder::startRecoding(const QString &recordPath)
{
    if (controller_.isRecording())
        return;

    controller_.startRecording(recordPath.toLocal8Bit().toStdString());
}

void Decoder::stopRecording()
{
    if (!controller_.isRecording())
        return;
    controller_.stopRecording();
}

void Decoder::seek(double pos)
{
    controller_.seek(pos);
}

void Decoder::setSpeed(double speed)
{
    controller_.setSpeed(speed);
}

void Decoder::setLoopMode(decoder_sdk::LoopMode mode, int maxLoops)
{
    controller_.setLoopMode(mode, maxLoops);
}

void Decoder::resetLoopCount()
{
    controller_.resetLoopCount();
}

bool Decoder::tryPopFrame(decoder_sdk::MediaType type, decoder_sdk::Frame &frame)
{
    switch (type) {
        case decoder_sdk::MediaType::kVideo:
            return controller_.videoQueue().tryPop(frame);
        case decoder_sdk::MediaType::kAudio:
            return controller_.audioQueue().tryPop(frame);
        default:
            break;
    }
    return false;
}

bool Decoder::frontFrame(decoder_sdk::MediaType type, decoder_sdk::Frame &frame)
{
    switch (type) {
        case decoder_sdk::MediaType::kVideo:
            return controller_.videoQueue().front(frame);
        case decoder_sdk::MediaType::kAudio:
            return controller_.audioQueue().front(frame);
        default:
            break;
    }
    return false;
}

double Decoder::frontPts(decoder_sdk::MediaType type)
{
    switch (type) {
        case decoder_sdk::MediaType::kVideo:
            return controller_.videoQueue().frontPts();
        case decoder_sdk::MediaType::kAudio:
            return controller_.audioQueue().frontPts();
        default:
            break;
    }
    return 0.0;
}

void Decoder::safeDelete()
{
    close();
    emit requestToDelete();
}

bool Decoder::pause()
{
    bool ret = controller_.pause();
    if (controller_.isRealTimeUrl())
        ret = controller_.stopDecode();
    return ret;
}

bool Decoder::resume()
{
    bool ret = false;
    if (controller_.isRealTimeUrl())
        ret = controller_.startDecode();
    ret = controller_.resume();
    return ret;
}

bool Decoder::close()
{
    if (controller_.isRecording())
        controller_.stopRecording();
    if (!controller_.isDecodeStopped())
        controller_.stopDecode();
    const auto ret = controller_.close();
    return ret;
}

void Decoder::openCallback(decoder_sdk::AsyncOpenResult result, bool openSuccess, const std::string &errorMessage)
{
    emit openResultReady(result == decoder_sdk::AsyncOpenResult::kSuccess, QString::fromStdString(errorMessage));
    if (result == decoder_sdk::AsyncOpenResult::kSuccess)
        controller_.startDecode();
}

void Decoder::streamEventCallback(decoder_sdk::EventType type, std::shared_ptr<decoder_sdk::EventArgs> event)
{
    emit eventUpdated(QString::fromStdString(controller_.url()), type, event);
}

#pragma endregion

#pragma region DecoderHelper
class DecoderHelper : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

signals:
    // 异步的打开流
    void requestToOpenAsync(const QString &url, const decoder_sdk::DecoderConfig &config);

    // 执行任务
    void requestToDoTask(DecodeWorker::Task t);

    // 广播流信息以及解码器信息
    void requestToBroadcastStreamAndDecoderInfo();

    // 开启录像，结果根据event变更进行处理
    void requestToStartRecoding(const QString &recordDir);
    // 关闭录像，结果根据event变更进行处理
    void requestToStopRecording();
    // 进行跳转
    void requestToSeek(double pos);
    // 设置倍速
    void requestToSetSpeed(double speed);

    // 设置循环模式
    void requestToSetLoopMode(decoder_sdk::LoopMode mode, int maxLoops = -1);
    // 重置循环计数
    void requestToResetLoopCount();
};
#pragma endregion

#pragma region DecodeWorker
DecodeWorker::DecodeWorker(const QString &key, QObject *parent)
    : IDecodeWorker(parent)
{
    // 创建解码器
    decoder_ = new Decoder;
    thread_ = new QThread(this);
    if (!key.isEmpty()) {
        thread_->setObjectName(QStringLiteral("decode_%1").arg(key));
    }
    decoder_->moveToThread(thread_);
    thread_->start();

    // 创建Helper
    helper_ = new DecoderHelper(this);

    // ============== Helper => Decoder的信号 ============== //
    connect(helper_, &DecoderHelper::requestToOpenAsync, decoder_.data(), &Decoder::openAsync);
    connect(helper_, &DecoderHelper::requestToDoTask, decoder_.data(), &Decoder::doTask, Qt::BlockingQueuedConnection);
    connect(helper_, &DecoderHelper::requestToBroadcastStreamAndDecoderInfo,
            decoder_.data(), &Decoder::broadcastStreamAndDecoderInfo);
    connect(helper_, &DecoderHelper::requestToStartRecoding, decoder_.data(), &Decoder::startRecoding);
    connect(helper_, &DecoderHelper::requestToStopRecording, decoder_.data(), &Decoder::stopRecording);
    connect(helper_, &DecoderHelper::requestToSeek, decoder_.data(), &Decoder::seek);
    connect(helper_, &DecoderHelper::requestToSetSpeed, decoder_.data(), &Decoder::setSpeed);
    connect(helper_, &DecoderHelper::requestToSetLoopMode, decoder_.data(), &Decoder::setLoopMode);
    connect(helper_, &DecoderHelper::requestToResetLoopCount, decoder_.data(), &Decoder::resetLoopCount);
    // ===================================================== //

    // ============== Decoder => Worker的信号 ============== //
    connect(decoder_.data(), &Decoder::openResultReady, this, &DecodeWorker::openResultReady);
    connect(decoder_.data(), &Decoder::requestToDelete, this, &DecodeWorker::requestToDelete);

    connect(decoder_.data(), &Decoder::eventUpdated, this, &DecodeWorker::eventUpdated);
    connect(decoder_.data(), &Decoder::streamInfoUpdated, this, &DecodeWorker::streamInfoUpdated);
    connect(decoder_.data(), &Decoder::decoderInfoUpdated, this, &DecodeWorker::decoderInfoUpdated);
    // ===================================================== //

    // ============ 线程析构时，同时析构decoder ============ //
    connect(thread_, &QThread::finished, decoder_, &Decoder::deleteLater);
    // ===================================================== //
}

DecodeWorker::~DecodeWorker()
{
    // decoder_->deleteLater();
    if (thread_->isRunning()) {
        thread_->requestInterruption();
        thread_->quit();
        thread_->wait();
    }

    // 加断言，确保decoder_在析构时被删除
    Q_ASSERT(decoder_.isNull());
}

void DecodeWorker::openAsync(const QString &url, const decoder_sdk::DecoderConfig &config)
{
    emit helper_->requestToOpenAsync(url, config);
}

void DecodeWorker::doTask(Task task)
{
    emit helper_->requestToDoTask(task);
}

void DecodeWorker::broadcastStreamAndDecoderInfo()
{
    emit helper_->requestToBroadcastStreamAndDecoderInfo();
}

void DecodeWorker::startRecoding(const QString &recordPath)
{
    emit helper_->requestToStartRecoding(recordPath);
}

void DecodeWorker::stopRecording()
{
    emit helper_->requestToStopRecording();
}

void DecodeWorker::seek(double pos)
{
    emit helper_->requestToSeek(pos);
}

void DecodeWorker::setSpeed(double speed)
{
    emit helper_->requestToSetSpeed(speed);
}

void DecodeWorker::setLoopMode(decoder_sdk::LoopMode mode, int maxLoops)
{
    emit helper_->requestToSetLoopMode(mode, maxLoops);
}

void DecodeWorker::resetLoopCount()
{
    emit helper_->requestToResetLoopCount();
}

bool DecodeWorker::tryPopFrame(decoder_sdk::MediaType type, decoder_sdk::Frame &frame)
{
    if (!decoder_)
        return false;

    return decoder_->tryPopFrame(type, frame);
}

bool DecodeWorker::frontFrame(decoder_sdk::MediaType type, decoder_sdk::Frame &frame)
{
    if (!decoder_)
        return false;

    return decoder_->frontFrame(type, frame);
}

double DecodeWorker::frontPts(decoder_sdk::MediaType type)
{
    if (!decoder_)
        return 0.0;

    return decoder_->frontPts(type);
}
#pragma endregion

#include "Decoder.moc"