#ifndef AUDIORENDER_H
#define AUDIORENDER_H

#include "PcmPushBuffer.h"

#include "decodersdk/audio_converter.h"
#include "decodersdk/frame.h"

#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioDevice>
#include <QAudioSink>
#else
#include <QAudioDeviceInfo>
#include <QAudioOutput>
#endif
#include <QAudioFormat>
#include <QIODevice>
#include <QObject>
#include <QScopedPointer>
#include <QTimer>

#include <atomic>
#include <vector>

class AudioRender : public QObject {
    Q_OBJECT

public:
    AudioRender(QObject *parent = nullptr);
    ~AudioRender() override;

    /**
     * @brief 初始化音频渲染器
     * @param frame 音频帧，用于获取音频格式信息
     * @param deviceInfo 音频设备信息，如果为空则使用默认设备
     */
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void initialize(const decoder_sdk::Frame &frame,
                    const QAudioDevice &deviceInfo = QAudioDevice());
#else
    void initialize(const decoder_sdk::Frame &frame,
                    const QAudioDeviceInfo &deviceInfo = QAudioDeviceInfo());
#endif

    /**
     * @brief 将音频帧写入 QAudioSink 的推模式缓冲区
     * @param frame 音频帧
     */
    void render(const decoder_sdk::Frame &frame);

    /**
     * @brief 开始播放
     */
    void start();

    /**
     * @brief 停止播放
     */
    void stop();

    /**
     * @brief 暂停播放
     */
    void pause();

    /**
     * @brief 恢复播放
     */
    void resume();

    /**
     * @brief 清空缓冲区并重置时钟（通常在跳转时调用）
     */
    void flush();

    /**
     * @brief 设置音量 (0.0 - 1.0)
     */
    void setVolume(qreal volume);

    /**
     * @brief 设置播放倍速
     * @param speed 播放倍速
     */
    void setSpeed(double speed);

    /**
     * @brief 获取音量
     */
    qreal volume() const;

    /**
     * @brief 是否有效
     */
    bool isValid() const;

    /**
     * @brief 获取当前播放状态
     */
    QAudio::State state() const;

    /**
     * @brief 获取设备硬件缓冲区空闲字节数
     */
    int bytesFree() const;

    /**
     * @brief 获取设备已播放的微秒数
     */
    qint64 processedUSecs() const;

    // 简化的统计信息
    struct Statistics {
        qint64 totalFramesRendered = 0;
        qint64 totalBytesRendered = 0;
        qint64 availableBytes = 0;
        qint64 droppedFrames = 0;
        qint64 droppedBytes = 0;
    };

    Statistics getStatistics() const;

signals:
    /**
     * @brief 首帧渲染信号（用于建立外部时钟基准）
     * @param pts 首帧 PTS
     * @param hardwareUSecs 此时设备已播放的微秒数
     */
    void firstFrameRendered(double pts, qint64 hardwareUSecs);
    /**
     * @brief 音频设备出现可写空间或缓冲耗尽时发出
     *
     * 该信号用于唤醒渲染线程重新检查 bytesFree/取音频帧，回调应保持轻量。
     */
    void bufferDrained();

private slots:
    /**
     * @brief 处理音频状态变化
     */
    void handleStateChanged(QAudio::State newState);

private:
    /**
     * @brief 将积压的 PCM 数据继续写入音频设备
     */
    void drainPendingPcm();

    /**
     * @brief 启动音频设备并更新播放状态
     * @return 是否启动成功
     */
    bool startAudioOutput();

    /**
     * @brief 根据实际写入结果更新统计信息
     */
    void updateStatistics(const PcmPushBuffer::WriteResult &result);

    /**
     * @brief 初始化音频格式
     * @param frame 音频帧
     * @return 是否成功
     */
    bool initAudioFormat(const decoder_sdk::Frame &frame);

    /**
     * @brief 初始化音频输出设备
     * @param deviceInfo 设备信息
     * @return 是否成功
     */
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool initAudioOutput(const QAudioDevice &deviceInfo);
#else
    bool initAudioOutput(const QAudioDeviceInfo &deviceInfo);
#endif

    /**
     * @brief 应用音量控制
     * @param data 音频数据
     * @param size 数据大小
     * @param volume 音量 (0.0 - 1.0)
     */
    void applyVolume(char *data, qint64 size, qreal volume);

    /**
     * @brief 从Frame获取音频通道数
     * @param frame 音频帧
     * @return 通道数
     */
    int getChannelCount(const decoder_sdk::Frame &frame);

    /**
     * @brief 将 PCM 写入 QAudioSink 内部设备
     * @param data PCM 数据
     * @param size 数据长度
     */
    void writeToAudioDevice(const char *data, qint64 size);

private:
    // 音频输出相关
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QScopedPointer<QAudioSink> audioOutput_;
#else
    QScopedPointer<QAudioOutput> audioOutput_;
#endif
    QIODevice *audioDevice_ = nullptr;
    PcmPushBuffer pendingPcm_;
    QTimer pushTimer_;
    QAudioFormat audioFormat_;
    decoder_sdk::AudioConverter audioConverter_;
    decoder_sdk::AudioSampleFormat outputSampleFormat_ = decoder_sdk::AudioSampleFormat::kUnknown;
    std::vector<uint8_t> convertedPcm_;
    int convertedSampleCount_ = 0;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QAudioDevice audioDeviceInfo_;
#else
    QAudioDeviceInfo audioDeviceInfo_;
#endif

    // 状态控制
    std::atomic<bool> initialized_;
    std::atomic<bool> playing_;
    std::atomic<bool> isFirstFrame_{ true };
    std::atomic<qreal> volume_;
    std::atomic<double> speed_{ 1.0 };

    // 音频格式信息
    int sampleRate_ = 44100;
    int channels_ = 2;
    int sampleSize_ = 16;

    // 统计信息
    // 统计信息
    std::atomic<qint64> totalFramesRendered_{ 0 };
    std::atomic<qint64> totalBytesRendered_{ 0 };
    std::atomic<qint64> droppedFrames_{ 0 };
    std::atomic<qint64> droppedBytes_{ 0 };

};

#endif // AUDIORENDER_H
