#include "AudioRender.h"

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

void AudioRender::initialize(const std::shared_ptr<decoder_sdk::Frame> &frame,
                             const QAudioDeviceInfo &deviceInfo)
{
    if (initialized_.load() || !frame || !frame->isValid()) {
        return;
    }

    if (!initAudioFormat(*frame)) {
        qWarning() << "[AudioRender] Failed to initialize audio format";
        return;
    }

    QAudioDeviceInfo device =
        deviceInfo.isNull() ? QAudioDeviceInfo::defaultOutputDevice() : deviceInfo;

    if (!initAudioOutput(device)) {
        qWarning() << "[AudioRender] Failed to initialize audio output";
        return;
    }

    initialized_.store(true);
    qDebug() << "[AudioRender] Initialized successfully"
             << "SampleRate:" << sampleRate_ << "Channels:" << channels_
             << "SampleSize:" << sampleSize_;
}

void AudioRender::render(const std::shared_ptr<decoder_sdk::Frame> &frame)
{
    if (!frame || !frame->isValid() || !isValid() || !audioDevice_) {
        return;
    }

    // 转换音频数据格式
    QByteArray audioData;
    int frameDataSize = convertAudioData(*frame, audioData);
    if (frameDataSize <= 0) {
        return;
    }

    // 应用音量控制
    applyVolume(audioData.data(), frameDataSize, volume_.load());

    // 直接写入设备，无需队列缓存
    qint64 bytesWritten = writeToDevice(audioData.constData(), audioData.size());

    // 统计信息更新
    if (bytesWritten > 0) {
        totalFramesRendered_.fetch_add(1);
        totalBytesRendered_.fetch_add(bytesWritten);
    }

    // 如果没有完全写入，记录丢弃
    if (bytesWritten < audioData.size()) {
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
            qWarning() << "[AudioRender] Failed to start audio output";
            return;
        }
        playing_.store(true);
        qDebug() << "[AudioRender] Started playing";
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

    qDebug() << "[AudioRender] Stopped playing";
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

void AudioRender::setVolume(qreal volume)
{
    volume_.store(qBound(0.0, volume, 1.0));
    if (audioOutput_) {
        audioOutput_->setVolume(volume_.load());
    }
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

void AudioRender::handleStateChanged(QAudio::State newState)
{
    if (newState == QAudio::StoppedState && audioOutput_ &&
        audioOutput_->error() != QAudio::NoError) {
        qWarning() << "[AudioRender] Audio error:" << audioOutput_->error();
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
            qWarning() << "[AudioRender] Unsupported sample format!";
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
        qWarning() << "[AudioRender] Audio format not supported by device";
        // 尝试获取最接近的格式
        audioFormat_ = audioDeviceInfo_.nearestFormat(audioFormat_);
        qDebug() << "[AudioRender] Using nearest format:"
                 << "SampleRate:" << audioFormat_.sampleRate()
                 << "Channels:" << audioFormat_.channelCount()
                 << "SampleSize:" << audioFormat_.sampleSize();
    }

    audioOutput_.reset(new QAudioOutput(audioDeviceInfo_, audioFormat_));
    if (!audioOutput_) {
        qWarning() << "[AudioRender] Failed to create QAudioOutput";
        return false;
    }

    // 设置较小的缓冲区大小以减少延迟
    int bytesPerSecond =
        audioFormat_.sampleRate() * audioFormat_.channelCount() * (audioFormat_.sampleSize() / 8);

    int targetBufferMs = 50; // 目标50ms缓冲，平衡延迟和稳定性
    int bufferSize = (bytesPerSecond * targetBufferMs) / 1000;

    // 确保缓冲区大小在合理范围内
    int minBufferSize = (bytesPerSecond * 20) / 1000;  // 最小20ms
    int maxBufferSize = (bytesPerSecond * 100) / 1000; // 最大100ms
    bufferSize = qBound(minBufferSize, bufferSize, maxBufferSize);

    audioOutput_->setBufferSize(bufferSize);

    // 通知间隔设置为缓冲区时长的一半，确保及时响应
    int notifyInterval = qMax(10, targetBufferMs / 2);
    audioOutput_->setNotifyInterval(notifyInterval);

    qDebug() << "[AudioRender] Audio output configured:"
             << "BufferSize:" << bufferSize << "bytes (" << (bufferSize * 1000 / bytesPerSecond)
             << "ms)"
             << "NotifyInterval:" << notifyInterval << "ms";

    // 连接信号
    connect(audioOutput_.data(), &QAudioOutput::stateChanged, this,
            &AudioRender::handleStateChanged);

    qDebug() << "[AudioRender] Audio output initialized";
    return true;
}

int AudioRender::convertAudioData(const decoder_sdk::Frame &frame, QByteArray &outputData)
{
    // 获取音频数据
    int channels = getChannelCount(frame);
    int64_t nbSamples = frame.nbSamples();

    // 交错格式（Interleaved），直接拷贝
    uint8_t *audioData = frame.data(0);
    int dataSize = frame.getAudioBufferSize();

    if (!audioData || dataSize <= 0) {
        return 0;
    }

    outputData.resize(dataSize);
    std::memcpy(outputData.data(), audioData, dataSize);
    return dataSize;
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
            qWarning() << "[AudioRender] Unsupported sample size for volume control:"
                       << sampleSize_;
            break;
    }
}

int AudioRender::getChannelCount(const decoder_sdk::Frame &frame)
{
    return frame.channels();
}

double AudioRender::getPts(const decoder_sdk::Frame &frame)
{
    // 使用secPts()方法获取时间戳
    return frame.secPts();
}

qint64 AudioRender::writeToDevice(const char *data, qint64 size)
{
    if (!audioDevice_ || !data || size <= 0) {
        return 0;
    }

    return audioDevice_->write(data, size);
}