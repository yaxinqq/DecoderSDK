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
    void initialize(const std::shared_ptr<decoder_sdk::Frame> &frame,
                    const QAudioDeviceInfo &deviceInfo = QAudioDeviceInfo());

    /**
     * @brief 渲染音频帧（非阻塞，立即处理）
     * @param frame 音频帧
     */
    void render(const std::shared_ptr<decoder_sdk::Frame> &frame);

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

    // 简化的统计信息
    struct Statistics {
        qint64 totalFramesRendered = 0;
        qint64 totalBytesRendered = 0;
        qint64 availableBytes = 0;
        qint64 droppedFrames = 0;
    };

    Statistics getStatistics() const;

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
     * @brief 转换音频数据格式
     * @param frame 输入音频帧
     * @param outputData 输出数据缓冲区
     * @return 转换后的数据大小
     */
    int convertAudioData(const decoder_sdk::Frame &frame, QByteArray &outputData);

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
     * @brief 从Frame获取PTS时间戳
     * @param frame 音频帧
     * @return PTS时间戳（秒）
     */
    double getPts(const decoder_sdk::Frame &frame);

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
    std::atomic<qreal> volume_;

    // 音频格式信息
    int sampleRate_ = 44100;
    int channels_ = 2;
    int sampleSize_ = 16;

    // 统计信息
    mutable std::atomic<qint64> totalFramesRendered_{0};
    mutable std::atomic<qint64> totalBytesRendered_{0};
    mutable std::atomic<qint64> droppedFrames_{0};
};

#endif // AUDIORENDER_H