#include "AudioRender.h"
#include "../Base/InternalUtils.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QMediaDevices>
#include <QAudioSink>
#endif

#include <QDebug>
#include <QThread>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace {

constexpr auto kAudioPushInterval = std::chrono::milliseconds(10);
constexpr int kMaxPcmPendingSeconds = 2;

decoder_sdk::AudioSampleFormat toAudioSampleFormat(const QAudioFormat &format)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    switch (format.sampleFormat()) {
        case QAudioFormat::UInt8:
            return decoder_sdk::AudioSampleFormat::kFmtU8;
        case QAudioFormat::Int16:
            return decoder_sdk::AudioSampleFormat::kFmtS16;
        case QAudioFormat::Int32:
            return decoder_sdk::AudioSampleFormat::kFmtS32;
        case QAudioFormat::Float:
            return decoder_sdk::AudioSampleFormat::kFmtFlt;
        default:
            return decoder_sdk::AudioSampleFormat::kUnknown;
    }
#else
    if (format.sampleType() == QAudioFormat::UnSignedInt && format.sampleSize() == 8) {
        return decoder_sdk::AudioSampleFormat::kFmtU8;
    }
    if (format.sampleType() == QAudioFormat::SignedInt && format.sampleSize() == 16) {
        return decoder_sdk::AudioSampleFormat::kFmtS16;
    }
    if (format.sampleType() == QAudioFormat::SignedInt && format.sampleSize() == 32) {
        return decoder_sdk::AudioSampleFormat::kFmtS32;
    }
    if (format.sampleType() == QAudioFormat::Float && format.sampleSize() == 32) {
        return decoder_sdk::AudioSampleFormat::kFmtFlt;
    }
    if (format.sampleType() == QAudioFormat::Float && format.sampleSize() == 64) {
        return decoder_sdk::AudioSampleFormat::kFmtDbl;
    }
    return decoder_sdk::AudioSampleFormat::kUnknown;
#endif
}

} // namespace

AudioRender::AudioRender(QObject *parent)
    : QObject(parent)
    , initialized_(false)
    , playing_(false)
    , volume_(1.0)
{
    pushTimer_.setInterval(kAudioPushInterval);
    connect(&pushTimer_, &QTimer::timeout, this, &AudioRender::drainPendingPcm);
}

AudioRender::~AudioRender()
{
    pushTimer_.stop();
    stop();
    if (audioOutput_) {
        audioOutput_->stop();
        audioOutput_.reset();
    }
    audioDevice_ = nullptr;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void AudioRender::initialize(const decoder_sdk::Frame &frame, const QAudioDevice &deviceInfo)
#else
void AudioRender::initialize(const decoder_sdk::Frame &frame, const QAudioDeviceInfo &deviceInfo)
#endif
{
    if (initialized_.load() || !frame.isValid()) {
        return;
    }

    if (!initAudioFormat(frame)) {
        qWarning() << QStringLiteral("[AudioRender] Failed to initialize audio format");
        return;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QAudioDevice device = deviceInfo.isNull() ? QMediaDevices::defaultAudioOutput() : deviceInfo;
#else
    QAudioDeviceInfo device =
        deviceInfo.isNull() ? QAudioDeviceInfo::defaultOutputDevice() : deviceInfo;
#endif

    if (!initAudioOutput(device)) {
        qWarning() << QStringLiteral("[AudioRender] Failed to initialize audio output");
        return;
    }

    sampleRate_ = audioFormat_.sampleRate();
    channels_ = audioFormat_.channelCount();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    sampleSize_ = audioFormat_.bytesPerSample() * 8;
#else
    sampleSize_ = audioFormat_.sampleSize();
#endif

    outputSampleFormat_ = toAudioSampleFormat(audioFormat_);
    if (outputSampleFormat_ == decoder_sdk::AudioSampleFormat::kUnknown ||
        !audioConverter_.configure(audioFormat_.sampleRate(), audioFormat_.channelCount(),
                                   outputSampleFormat_)) {
        qWarning() << QStringLiteral("[AudioRender] Failed to configure audio converter");
        return;
    }
    if (!audioConverter_.setSpeed(speed_.load())) {
        qWarning() << QStringLiteral("[AudioRender] Failed to apply playback speed");
        return;
    }

    const qint64 maxPendingBytes = static_cast<qint64>(audioFormat_.sampleRate()) *
                                   audioFormat_.channelCount() * (sampleSize_ / 8) * kMaxPcmPendingSeconds;
    pendingPcm_.setMaxPendingBytes(static_cast<std::size_t>(maxPendingBytes));

    initialized_.store(true);
    qDebug() << QStringLiteral(
                    "[AudioRender] Initialized successfully! SampleRate: %1, Channels: %2, "
                    "SampleSize: %3")
                    .arg(QString::number(sampleRate_), QString::number(channels_),
                         QString::number(sampleSize_));
}

void AudioRender::render(const decoder_sdk::Frame &frame)
{
    if (!frame.isValid() || !isValid() || !playing_.load()) {
        return;
    }

    // 记录首帧PTS并发出信号，用于建立音频时钟
    if (isFirstFrame_.load()) {
        isFirstFrame_.store(false);
        emit firstFrameRendered(frame.secPts(), audioOutput_ ? audioOutput_->processedUSecs() : 0);
    }

    const qreal currentVolume = volume_.load();
    if (std::abs(speed_.load() - 1.0) >= 0.0001 ||
        frame.sampleRate() != audioFormat_.sampleRate() ||
        frame.channels() != audioFormat_.channelCount() ||
        frame.sampleFormat() != outputSampleFormat_) {
        if (!audioConverter_.convert(frame, convertedPcm_, convertedSampleCount_)) {
            qWarning() << QStringLiteral("[AudioRender] Failed to convert audio frame");
            return;
        }

        applyVolume(reinterpret_cast<char *>(convertedPcm_.data()),
                    static_cast<qint64>(convertedPcm_.size()), currentVolume);
        writeToAudioDevice(reinterpret_cast<const char *>(convertedPcm_.data()),
                           static_cast<qint64>(convertedPcm_.size()));
    } else {
        const int dataSize = frame.getAudioBufferSize();
        uint8_t *audioData = frame.data(0);
        if (!audioData || dataSize <= 0) {
            return;
        }

        if (!qFuzzyCompare(currentVolume, 1.0)) {
            convertedPcm_.assign(audioData, audioData + dataSize);
            applyVolume(reinterpret_cast<char *>(convertedPcm_.data()),
                        static_cast<qint64>(convertedPcm_.size()), currentVolume);
            writeToAudioDevice(reinterpret_cast<const char *>(convertedPcm_.data()),
                               static_cast<qint64>(convertedPcm_.size()));
        } else {
            writeToAudioDevice(reinterpret_cast<const char *>(audioData), dataSize);
        }
    }
}

void AudioRender::start()
{
    if (!isValid() || playing_.load()) {
        return;
    }

    if (startAudioOutput()) {
        qDebug() << QStringLiteral("[AudioRender] Started playing");
    }
}

void AudioRender::stop()
{
    playing_.store(false);
    pushTimer_.stop();

    if (audioOutput_) {
        audioOutput_->stop();
        audioDevice_ = nullptr;
    }

    pendingPcm_.clear();
    audioConverter_.reset();
    convertedPcm_.clear();
    convertedSampleCount_ = 0;

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
        pushTimer_.stop();
    }
}

void AudioRender::resume()
{
    if (!playing_.load()) {
        return;
    }

    if (audioOutput_) {
        audioOutput_->resume();
        if (playing_.load()) {
            pushTimer_.start(kAudioPushInterval);
        }
    }
}

void AudioRender::flush()
{
    const bool wasPlaying = playing_.exchange(false);
    pushTimer_.stop();

    if (audioOutput_) {
        audioOutput_->reset();
        audioDevice_ = nullptr;
    }

    pendingPcm_.clear();
    audioConverter_.reset();
    convertedPcm_.clear();
    convertedSampleCount_ = 0;

    if (wasPlaying && !startAudioOutput()) {
        qWarning() << QStringLiteral("[AudioRender] Failed to restart audio output");
    }

    // 重置时钟状态
    isFirstFrame_.store(true);

    qDebug() << QStringLiteral("[AudioRender] Flushed and clock reset");
}

void AudioRender::setVolume(qreal volume)
{
    volume_.store(std::clamp(volume, 0.0, 1.0));
}

void AudioRender::setSpeed(double speed)
{
    if (!std::isfinite(speed) || !utils::greater(speed, 0.0) || utils::greater(speed, 64.0)) {
        qWarning() << QStringLiteral("[AudioRender] Invalid playback speed: %1").arg(speed);
        return;
    }

    speed_.store(speed);
    if (initialized_.load() && !audioConverter_.setSpeed(speed)) {
        qWarning() << QStringLiteral("[AudioRender] Failed to update playback speed");
    }
}

void AudioRender::writeToAudioDevice(const char *data, qint64 size)
{
    if (!audioDevice_ || playing_.load() == false || !data || size <= 0) {
        return;
    }

    const auto result = pendingPcm_.write(
        reinterpret_cast<const std::uint8_t *>(data), static_cast<std::size_t>(size),
        [this](const std::uint8_t *pcmData, std::size_t pcmSize) {
            return audioDevice_->write(reinterpret_cast<const char *>(pcmData),
                                       static_cast<qint64>(pcmSize));
        });
    updateStatistics(result);
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
    stats.droppedBytes = droppedBytes_.load();
    stats.availableBytes = static_cast<qint64>(pendingPcm_.pendingBytes());
    return stats;
}

QAudio::State AudioRender::state() const
{
    return audioOutput_ ? audioOutput_->state() : QAudio::StoppedState;
}

int AudioRender::bytesFree() const
{
    if (!isValid() || !audioDevice_) {
        return 0;
    }

    const qint64 sinkBytesFree =
        (std::max)(static_cast<qint64>(0), static_cast<qint64>(audioOutput_->bytesFree()));
    const std::size_t effectiveBytes =
        pendingPcm_.effectiveBytesFree(static_cast<std::size_t>(sinkBytesFree));
    return static_cast<int>(
        (std::min)(effectiveBytes, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
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
        playing_.store(false);
        pushTimer_.stop();
    }

    // 推模式下，设备空闲表示内部缓冲已耗尽，先排空应用层残留，再通知生产者。
    if (newState == QAudio::IdleState && playing_.load()) {
        drainPendingPcm();
    }
}

void AudioRender::drainPendingPcm()
{
    if (!audioDevice_ || !playing_.load()) {
        return;
    }

    const auto result = pendingPcm_.drain(
        [this](const std::uint8_t *pcmData, std::size_t pcmSize) {
            return audioDevice_->write(reinterpret_cast<const char *>(pcmData),
                                       static_cast<qint64>(pcmSize));
        });
    updateStatistics(result);

    // QAudioSink 在缓冲区未耗尽时不会通知生产者，定时检查可写空间以保持连续供给。
    if (bytesFree() > 0) {
        emit bufferDrained();
    }
}

bool AudioRender::startAudioOutput()
{
    if (!audioOutput_) {
        return false;
    }

    playing_.store(false);
    audioDevice_ = audioOutput_->start();
    if (!audioDevice_ || audioOutput_->error() != QAudio::NoError) {
        qWarning() << QStringLiteral("[AudioRender] Failed to start audio output");
        audioOutput_->stop();
        audioDevice_ = nullptr;
        pushTimer_.stop();
        return false;
    }

    playing_.store(true);
    pushTimer_.start(kAudioPushInterval);
    return true;
}

void AudioRender::updateStatistics(const PcmPushBuffer::WriteResult &result)
{
    totalFramesRendered_.fetch_add(static_cast<qint64>(result.completedFrames));
    totalBytesRendered_.fetch_add(static_cast<qint64>(result.acceptedBytes));
    droppedFrames_.fetch_add(static_cast<qint64>(result.droppedFrames));
    droppedBytes_.fetch_add(static_cast<qint64>(result.droppedBytes));
}

bool AudioRender::initAudioFormat(const decoder_sdk::Frame &frame)
{
    // 从Frame获取音频参数
    sampleRate_ = frame.sampleRate();
    channels_ = getChannelCount(frame);

    // 根据FFmpeg的采样格式设置Qt音频格式
    const auto sampleFormat = frame.sampleFormat();

    // 设置Qt音频格式
    audioFormat_.setSampleRate((std::max)(sampleRate_, 1));
    audioFormat_.setChannelCount((std::max)(channels_, 1));
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    audioFormat_.setCodec("audio/pcm");
    audioFormat_.setByteOrder(QAudioFormat::LittleEndian);
#endif

    // 根据FFmpeg采样格式设置Qt格式
    switch (sampleFormat) {
        case decoder_sdk::AudioSampleFormat::kFmtU8:
        case decoder_sdk::AudioSampleFormat::kFmtU8P:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            audioFormat_.setSampleFormat(QAudioFormat::UInt8);
#else
            audioFormat_.setSampleSize(8);
            audioFormat_.setSampleType(QAudioFormat::UnSignedInt);
#endif
            sampleSize_ = 8;
            break;

        case decoder_sdk::AudioSampleFormat::kFmtS16:
        case decoder_sdk::AudioSampleFormat::kFmtS16P:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            audioFormat_.setSampleFormat(QAudioFormat::Int16);
#else
            audioFormat_.setSampleSize(16);
            audioFormat_.setSampleType(QAudioFormat::SignedInt);
#endif
            sampleSize_ = 16;
            break;

        case decoder_sdk::AudioSampleFormat::kFmtS32:
        case decoder_sdk::AudioSampleFormat::kFmtS32P:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            audioFormat_.setSampleFormat(QAudioFormat::Int32);
#else
            audioFormat_.setSampleSize(32);
            audioFormat_.setSampleType(QAudioFormat::SignedInt);
#endif
            sampleSize_ = 32;
            break;

        case decoder_sdk::AudioSampleFormat::kFmtFlt:
        case decoder_sdk::AudioSampleFormat::kFmtFltP:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            audioFormat_.setSampleFormat(QAudioFormat::Float);
#else
            audioFormat_.setSampleSize(32);
            audioFormat_.setSampleType(QAudioFormat::Float);
#endif
            sampleSize_ = 32;
            break;

        case decoder_sdk::AudioSampleFormat::kFmtDbl:
        case decoder_sdk::AudioSampleFormat::kFmtDblP:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            qWarning() << QStringLiteral(
                "[AudioRender] 64-bit float sample format is not supported by Qt6");
            return false;
#else
            audioFormat_.setSampleSize(64);
            audioFormat_.setSampleType(QAudioFormat::Float);
            sampleSize_ = 64;
            break;
#endif

        default:
            qWarning() << QStringLiteral("[AudioRender] Unsupported sample format!");
            // 默认使用16位有符号整数
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            audioFormat_.setSampleFormat(QAudioFormat::Int16);
#else
            audioFormat_.setSampleSize(16);
            audioFormat_.setSampleType(QAudioFormat::SignedInt);
#endif
            sampleSize_ = 16;
            break;
    }

    return true;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool AudioRender::initAudioOutput(const QAudioDevice &deviceInfo)
#else
bool AudioRender::initAudioOutput(const QAudioDeviceInfo &deviceInfo)
#endif
{
    audioDeviceInfo_ = deviceInfo;

    // 检查设备是否支持我们的音频格式
    if (!audioDeviceInfo_.isFormatSupported(audioFormat_)) {
        qWarning() << QStringLiteral("[AudioRender] Audio format not supported by device");
        // 尝试获取最接近的格式
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        audioFormat_ = audioDeviceInfo_.preferredFormat();
#else
        audioFormat_ = audioDeviceInfo_.nearestFormat(audioFormat_);
#endif
        qDebug() << QStringLiteral(
                        "[AudioRender] Using nearest format: SampleRate: %1, Channels: %2, "
                        "SampleSize: %3")
                        .arg(QString::number(audioFormat_.sampleRate()),
                             QString::number(audioFormat_.channelCount()),
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                             QString::number(audioFormat_.bytesPerSample() * 8));
#else
                             QString::number(audioFormat_.sampleSize()));
#endif
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    audioOutput_.reset(new QAudioSink(audioDeviceInfo_, audioFormat_));
#else
    audioOutput_.reset(new QAudioOutput(audioDeviceInfo_, audioFormat_));
#endif
    if (!audioOutput_) {
        qWarning() << QStringLiteral("[AudioRender] Failed to create audio output");
        return false;
    }

    // 设置较小的缓冲区大小以减少延迟
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    int bytesPerSecond =
        audioFormat_.sampleRate() * audioFormat_.channelCount() * audioFormat_.bytesPerSample();
#else
    int bytesPerSecond =
        audioFormat_.sampleRate() * audioFormat_.channelCount() * (audioFormat_.sampleSize() / 8);
#endif
    constexpr auto targetBuffer = std::chrono::milliseconds(200);
    int bufferSize = bytesPerSecond * (targetBuffer / std::chrono::milliseconds(1)) / 1000;

    // 确保缓冲区大小在合理范围内
    constexpr auto minBuffer = std::chrono::milliseconds(50);
    constexpr auto maxBuffer = std::chrono::milliseconds(400);
    const int minBufferSize =
        bytesPerSecond * (minBuffer / std::chrono::milliseconds(1)) / 1000;
    const int maxBufferSize =
        bytesPerSecond * (maxBuffer / std::chrono::milliseconds(1)) / 1000;
    bufferSize = std::clamp(bufferSize, minBufferSize, maxBufferSize);

    audioOutput_->setBufferSize(bufferSize);

    qDebug() << QStringLiteral(
                    "[AudioRender] Audio output configured: BufferSize: %1 bytes (%2 ms)")
                    .arg(QString::number(bufferSize),
                         QString::number(bufferSize * 1000 / bytesPerSecond));

    // 连接信号
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    connect(audioOutput_.data(), &QAudioSink::stateChanged, this,
            &AudioRender::handleStateChanged);
#else
    connect(audioOutput_.data(), &QAudioOutput::stateChanged, this,
            &AudioRender::handleStateChanged);
#endif

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
                samples[i] = static_cast<uint8_t>(std::clamp(value, 0, 255));
            }
            break;
        }
        case 16: {
            // 16位有符号整数
            int16_t *samples = reinterpret_cast<int16_t *>(data);
            qint64 sampleCount = size / sizeof(int16_t);
            for (qint64 i = 0; i < sampleCount; ++i) {
                int value = samples[i] * volume;
                samples[i] =
                    static_cast<int16_t>(std::clamp(value, -32768, 32767));
            }
            break;
        }
        case 32: {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            if (audioFormat_.sampleFormat() == QAudioFormat::Int32) {
#else
            if (audioFormat_.sampleType() == QAudioFormat::SignedInt) {
#endif
                // 32位有符号整数
                int32_t *samples = reinterpret_cast<int32_t *>(data);
                qint64 sampleCount = size / sizeof(int32_t);
                for (qint64 i = 0; i < sampleCount; ++i) {
                    int64_t value = static_cast<int64_t>(samples[i]) * volume;
                    samples[i] = static_cast<int32_t>(
                        std::clamp(value, static_cast<int64_t>(INT32_MIN),
                                   static_cast<int64_t>(INT32_MAX)));
                }
            } else {
                // 32位浮点数
                float *samples = reinterpret_cast<float *>(data);
                qint64 sampleCount = size / sizeof(float);
                for (qint64 i = 0; i < sampleCount; ++i) {
                    samples[i] =
                        std::clamp(static_cast<float>(samples[i] * volume), -1.0f, 1.0f);
                }
            }
            break;
        }
        case 64: {
            // 64位浮点数
            double *samples = reinterpret_cast<double *>(data);
            qint64 sampleCount = size / sizeof(double);
            for (qint64 i = 0; i < sampleCount; ++i) {
                samples[i] = std::clamp(samples[i] * volume, -1.0, 1.0);
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
