#include "AudioRender.h"
#include "../Base/InternalUtils.h"

#include <QAudioDeviceInfo>
#include <QDebug>
#include <QThread>
#include <algorithm>
#include <cstring>

AudioRender::AudioRender(QObject *parent)
    : QObject(parent), initialized_(false), playing_(false), volume_(1.0)
{
}

AudioRender::~AudioRender()
{
    stop();
    if (audioOutput_) {
        audioOutput_->stop();
        audioOutput_.reset();
    }
}

void AudioRender::initialize(const decoder_sdk::Frame &frame, const QAudioDeviceInfo &deviceInfo)
{
    if (initialized_.load() || !frame.isValid()) {
        return;
    }

    if (!initAudioFormat(frame)) {
        qWarning() << QStringLiteral("[AudioRender] Failed to initialize audio format");
        return;
    }

    QAudioDeviceInfo device =
        deviceInfo.isNull() ? QAudioDeviceInfo::defaultOutputDevice() : deviceInfo;

    if (!initAudioOutput(device)) {
        qWarning() << QStringLiteral("[AudioRender] Failed to initialize audio output");
        return;
    }

    initialized_.store(true);
    qDebug() << QStringLiteral(
                    "[AudioRender] Initialized successfully! SampleRate: %1, Channels: %2, "
                    "SampleSize: %3")
                    .arg(QString::number(sampleRate_), QString::number(channels_),
                         QString::number(sampleSize_));
}

void AudioRender::render(const decoder_sdk::Frame &frame)
{
    if (!frame.isValid() || !isValid() || !audioDevice_) {
        return;
    }

    std::lock_guard<std::mutex> lock(bufferMutex_);

    // 记录首帧PTS并发出信号，用于建立音频时钟
    if (isFirstFrame_.load()) {
        isFirstFrame_.store(false);
        residualBuffer_.clear();
        emit firstFrameRendered(frame.secPts(), audioOutput_ ? audioOutput_->processedUSecs() : 0);
    }

    // 1. 尝试写入之前积压的残余数据
    if (!residualBuffer_.isEmpty()) {
        qint64 written = writeToDevice(residualBuffer_.constData(), residualBuffer_.size());
        if (written > 0) {
            residualBuffer_.remove(0, written);
        }
    }

    // 如果残余数据还是太多，则先不处理新帧，等待下一轮
    if (residualBuffer_.size() > 65536) {
        return;
    }

    // 2. 获取当前音频帧数据
    int dataSize = frame.getAudioBufferSize();
    uint8_t *audioData = frame.data(0);
    if (!audioData || dataSize <= 0) {
        return;
    }

    // 3. 将新数据追加到残余缓冲区并统一尝试写入
    residualBuffer_.append(reinterpret_cast<const char *>(audioData), dataSize);

    qint64 written = writeToDevice(residualBuffer_.constData(), residualBuffer_.size());
    if (written > 0) {
        residualBuffer_.remove(0, written);

        // 统计信息更新
        totalFramesRendered_.fetch_add(1);
        totalBytesRendered_.fetch_add(written);
    }

    // 缓冲区积压超过 128KB (约 0.7s) 强制清空，防止内存无限增长
    if (residualBuffer_.size() > 131072) {
        residualBuffer_.clear();
        droppedFrames_.fetch_add(1);
    }
}

void AudioRender::start()
{
    if (!isValid() || playing_.load()) {
        return;
    }

    if (audioOutput_) {
        audioDevice_ = audioOutput_->start();
        if (!audioDevice_) {
            qWarning() << QStringLiteral("[AudioRender] Failed to start audio output");
            return;
        }
        playing_.store(true);
        qDebug() << QStringLiteral("[AudioRender] Started playing");
    }
}

void AudioRender::stop()
{
    if (!playing_.load()) {
        return;
    }

    playing_.store(false);

    if (audioOutput_) {
        audioOutput_->stop();
        audioDevice_ = nullptr;
    }

    // 重置时钟状态
    isFirstFrame_.store(true);

    qDebug() << QStringLiteral("[AudioRender] Stopped playing");
}

void AudioRender::pause()
{
    if (!playing_.load()) {
        return;
    }

    if (audioOutput_) {
        audioOutput_->suspend();
    }
}

void AudioRender::resume()
{
    if (!playing_.load()) {
        return;
    }

    if (audioOutput_) {
        audioOutput_->resume();
    }
}

void AudioRender::flush()
{
    std::lock_guard<std::mutex> lock(bufferMutex_);
    if (audioOutput_) {
        audioOutput_->reset();
        // reset() 后需要重新获取设备指针，否则可能写入失败
        audioDevice_ = audioOutput_->start();
    }

    // 重置时钟状态与缓冲区
    isFirstFrame_.store(true);
    residualBuffer_.clear();

    qDebug() << QStringLiteral("[AudioRender] Flushed and clock reset");
}

void AudioRender::setVolume(qreal volume)
{
    volume_.store(qBound(0.0, volume, 1.0));
}

qreal AudioRender::volume() const
{
    return volume_.load();
}

bool AudioRender::isValid() const
{
    return initialized_.load() && audioOutput_;
}

AudioRender::Statistics AudioRender::getStatistics() const
{
    Statistics stats;
    stats.totalFramesRendered = totalFramesRendered_.load();
    stats.totalBytesRendered = totalBytesRendered_.load();

    stats.droppedFrames = droppedFrames_.load();
    return stats;
}

QAudio::State AudioRender::state() const
{
    return audioOutput_ ? audioOutput_->state() : QAudio::StoppedState;
}

int AudioRender::bytesFree() const
{
    return audioOutput_ ? audioOutput_->bytesFree() : 0;
}

qint64 AudioRender::processedUSecs() const
{
    return audioOutput_ ? audioOutput_->processedUSecs() : 0;
}

void AudioRender::handleStateChanged(QAudio::State newState)
{
    if (newState == QAudio::StoppedState && audioOutput_ &&
        audioOutput_->error() != QAudio::NoError) {
        qWarning() << QStringLiteral("[AudioRender] Audio error: %1")
                          .arg(static_cast<int>(audioOutput_->error()));
    }

    // 处理IdleState：当音频设备进入空闲状态时，检查是否有更多数据
    if (newState == QAudio::IdleState && playing_.load()) {
        audioOutput_->resume();
    }
}

bool AudioRender::initAudioFormat(const decoder_sdk::Frame &frame)
{
    // 从Frame获取音频参数
    sampleRate_ = frame.sampleRate();
    channels_ = getChannelCount(frame);

    // 根据FFmpeg的采样格式设置Qt音频格式
    const auto sampleFormat = frame.sampleFormat();

    // 设置Qt音频格式
    audioFormat_.setSampleRate(std::max(sampleRate_, 1));
    audioFormat_.setChannelCount(std::max(channels_, 1));
    audioFormat_.setCodec("audio/pcm");
    audioFormat_.setByteOrder(QAudioFormat::LittleEndian);

    // 根据FFmpeg采样格式设置Qt格式
    switch (sampleFormat) {
        case decoder_sdk::AudioSampleFormat::kFmtU8:
        case decoder_sdk::AudioSampleFormat::kFmtU8P:
            audioFormat_.setSampleSize(8);
            audioFormat_.setSampleType(QAudioFormat::UnSignedInt);
            sampleSize_ = 8;
            break;

        case decoder_sdk::AudioSampleFormat::kFmtS16:
        case decoder_sdk::AudioSampleFormat::kFmtS16P:
            audioFormat_.setSampleSize(16);
            audioFormat_.setSampleType(QAudioFormat::SignedInt);
            sampleSize_ = 16;
            break;

        case decoder_sdk::AudioSampleFormat::kFmtS32:
        case decoder_sdk::AudioSampleFormat::kFmtS32P:
            audioFormat_.setSampleSize(32);
            audioFormat_.setSampleType(QAudioFormat::SignedInt);
            sampleSize_ = 32;
            break;

        case decoder_sdk::AudioSampleFormat::kFmtFlt:
        case decoder_sdk::AudioSampleFormat::kFmtFltP:
            audioFormat_.setSampleSize(32);
            audioFormat_.setSampleType(QAudioFormat::Float);
            sampleSize_ = 32;
            break;

        case decoder_sdk::AudioSampleFormat::kFmtDbl:
        case decoder_sdk::AudioSampleFormat::kFmtDblP:
            audioFormat_.setSampleSize(64);
            audioFormat_.setSampleType(QAudioFormat::Float);
            sampleSize_ = 64;
            break;

        default:
            qWarning() << QStringLiteral("[AudioRender] Unsupported sample format!");
            // 默认使用16位有符号整数
            audioFormat_.setSampleSize(16);
            audioFormat_.setSampleType(QAudioFormat::SignedInt);
            sampleSize_ = 16;
            break;
    }

    return true;
}

bool AudioRender::initAudioOutput(const QAudioDeviceInfo &deviceInfo)
{
    audioDeviceInfo_ = deviceInfo;

    // 检查设备是否支持我们的音频格式
    if (!audioDeviceInfo_.isFormatSupported(audioFormat_)) {
        qWarning() << QStringLiteral("[AudioRender] Audio format not supported by device");
        // 尝试获取最接近的格式
        audioFormat_ = audioDeviceInfo_.nearestFormat(audioFormat_);
        qDebug() << QStringLiteral(
                        "[AudioRender] Using nearest format: SampleRate: %1, Channels: %2, "
                        "SampleSize: %3")
                        .arg(QString::number(audioFormat_.sampleRate()),
                             QString::number(audioFormat_.channelCount()),
                             QString::number(audioFormat_.sampleSize()));
    }

    audioOutput_.reset(new QAudioOutput(audioDeviceInfo_, audioFormat_));
    if (!audioOutput_) {
        qWarning() << QStringLiteral("[AudioRender] Failed to create QAudioOutput");
        return false;
    }

    // 设置较小的缓冲区大小以减少延迟
    int bytesPerSecond =
        audioFormat_.sampleRate() * audioFormat_.channelCount() * (audioFormat_.sampleSize() / 8);

    int targetBufferMs = 200; // 增加到 200ms 缓冲，提高稳定性，消除颤抖感
    int bufferSize = (bytesPerSecond * targetBufferMs) / 1000;

    // 确保缓冲区大小在合理范围内
    int minBufferSize = (bytesPerSecond * 50) / 1000;  // 最小 50ms
    int maxBufferSize = (bytesPerSecond * 400) / 1000; // 最大 400ms
    bufferSize = qBound(minBufferSize, bufferSize, maxBufferSize);

    audioOutput_->setBufferSize(bufferSize);

    // 通知间隔设置为缓冲区时长的一半，确保及时响应
    int notifyInterval = qMax(10, targetBufferMs / 2);
    audioOutput_->setNotifyInterval(notifyInterval);

    qDebug() << QStringLiteral(
                    "[AudioRender] Audio output configured: BufferSize: %1 bytes (%2 ms), "
                    "NotifyInterval: %3 ms")
                    .arg(QString::number(bufferSize),
                         QString::number(bufferSize * 1000 / bytesPerSecond),
                         QString::number(notifyInterval));

    // 连接信号
    connect(audioOutput_.data(), &QAudioOutput::stateChanged, this,
            &AudioRender::handleStateChanged);

    qDebug() << QStringLiteral("[AudioRender] Audio output initialized");
    return true;
}

void AudioRender::applyVolume(char *data, qint64 size, qreal volume)
{
    if (qFuzzyCompare(volume, 1.0) || size <= 0) {
        return;
    }

    // 根据采样格式应用音量
    switch (sampleSize_) {
        case 8: {
            // 8位无符号整数
            uint8_t *samples = reinterpret_cast<uint8_t *>(data);
            qint64 sampleCount = size / sizeof(uint8_t);
            for (qint64 i = 0; i < sampleCount; ++i) {
                int value = (samples[i] - 128) * volume + 128;
                samples[i] = static_cast<uint8_t>(qBound(0, value, 255));
            }
            break;
        }
        case 16: {
            // 16位有符号整数
            int16_t *samples = reinterpret_cast<int16_t *>(data);
            qint64 sampleCount = size / sizeof(int16_t);
            for (qint64 i = 0; i < sampleCount; ++i) {
                int value = samples[i] * volume;
                samples[i] = static_cast<int16_t>(qBound(-32768, value, 32767));
            }
            break;
        }
        case 32: {
            if (audioFormat_.sampleType() == QAudioFormat::SignedInt) {
                // 32位有符号整数
                int32_t *samples = reinterpret_cast<int32_t *>(data);
                qint64 sampleCount = size / sizeof(int32_t);
                for (qint64 i = 0; i < sampleCount; ++i) {
                    int64_t value = static_cast<int64_t>(samples[i]) * volume;
                    samples[i] = static_cast<int32_t>(qBound(static_cast<int64_t>(INT32_MIN), value,
                                                             static_cast<int64_t>(INT32_MAX)));
                }
            } else {
                // 32位浮点数
                float *samples = reinterpret_cast<float *>(data);
                qint64 sampleCount = size / sizeof(float);
                for (qint64 i = 0; i < sampleCount; ++i) {
                    samples[i] = qBound(-1.0f, static_cast<float>(samples[i] * volume), 1.0f);
                }
            }
            break;
        }
        case 64: {
            // 64位浮点数
            double *samples = reinterpret_cast<double *>(data);
            qint64 sampleCount = size / sizeof(double);
            for (qint64 i = 0; i < sampleCount; ++i) {
                samples[i] = qBound(-1.0, samples[i] * volume, 1.0);
            }
            break;
        }
        default:
            qWarning() << QStringLiteral(
                              "[AudioRender] Unsupported sample size for volume control: %1")
                              .arg(QString::number(sampleSize_));
            break;
    }
}

int AudioRender::getChannelCount(const decoder_sdk::Frame &frame)
{
    return frame.channels();
}

qint64 AudioRender::writeToDevice(const char *data, qint64 size)
{
    if (!audioDevice_ || !data || size <= 0) {
        return 0;
    }

    return audioDevice_->write(data, size);
}