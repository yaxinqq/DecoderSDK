#ifndef AUDIORENDER_H
#define AUDIORENDER_H

#include "decodersdk/frame.h"

#include <QAudioDeviceInfo>
#include <QAudioFormat>
#include <QAudioOutput>
#include <QByteArray>
#include <QObject>
#include <QScopedPointer>
#include <QTimer>
#include <atomic>
#include <memory>
#include <mutex>
#include <queue>

class AudioRender : public QObject {
    Q_OBJECT

public:
    AudioRender(QObject *parent = nullptr);
    virtual ~AudioRender();

    /**
     * @brief 初始化音频渲染器
     * @param frame 音频帧，用于获取音频格式信息
     * @param deviceInfo 音频设备信息，如果为空则使用默认设备
     */
    void initialize(const decoder_sdk::Frame &frame,
                    const QAudioDeviceInfo &deviceInfo = QAudioDeviceInfo());

    /**
     * @brief 渲染音频帧（非阻塞，立即处理）
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
    };

    Statistics getStatistics() const;

signals:
    /**
     * @brief 首帧渲染信号（用于建立外部时钟基准）
     * @param pts 首帧 PTS
     * @param hardwareUSecs 此时设备已播放的微秒数
     */
    void firstFrameRendered(double pts, qint64 hardwareUSecs);

private slots:
    /**
     * @brief 处理音频状态变化
     */
    void handleStateChanged(QAudio::State newState);

private:
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
    bool initAudioOutput(const QAudioDeviceInfo &deviceInfo);

    /**
     * @brief 移除音频渲染器中的格式转换，因为解码库已保证格式正确
     */
    // int convertAudioData(const decoder_sdk::Frame &frame, QByteArray &outputData);

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
     * @brief 将数据写入音频设备
     * @param data 音频数据
     * @param size 数据大小
     * @return 实际写入的字节数
     */
    qint64 writeToDevice(const char *data, qint64 size);

private:
    // 音频输出相关
    QScopedPointer<QAudioOutput> audioOutput_;
    QIODevice *audioDevice_ = nullptr;
    QAudioFormat audioFormat_;
    QAudioDeviceInfo audioDeviceInfo_;

    // 状态控制
    std::atomic<bool> initialized_;
    std::atomic<bool> playing_;
    std::atomic<bool> isFirstFrame_{true};
    std::atomic<qreal> volume_;

    // 音频格式信息
    int sampleRate_ = 44100;
    int channels_ = 2;
    int sampleSize_ = 16;

    // 统计信息
    // 统计信息
    std::atomic<qint64> totalFramesRendered_{0};
    std::atomic<qint64> totalBytesRendered_{0};
    std::atomic<qint64> droppedFrames_{0};

    // 残余数据缓冲区，处理部分写入问题
    QByteArray residualBuffer_;
    std::mutex bufferMutex_;
};

#endif // AUDIORENDER_H