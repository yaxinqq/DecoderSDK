#ifndef AUDIOBUFFERQUEUE_H
#define AUDIOBUFFERQUEUE_H

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QMutex>
#include <QWaitCondition>
#include <atomic>
#include <queue>
#include <vector>

/**
 * @brief 音频缓冲区项
 */
struct AudioBufferItem {
    QByteArray data;
    qint64 timestamp = 0;
    qint64 enqueueTime = 0;

    AudioBufferItem() = default;
    AudioBufferItem(const QByteArray &audioData, qint64 ts)
        : data(audioData), timestamp(ts), enqueueTime(QDateTime::currentMSecsSinceEpoch())
    {
    }
};

/**
 * @brief 音频缓冲队列
 */
class AudioBufferQueue {
public:
    explicit AudioBufferQueue(int maxBuffers = 15);
    ~AudioBufferQueue();

    /**
     * @brief 初始化缓冲队列
     * @param sampleRate 采样率
     * @param channels 通道数
     * @param sampleSize 采样位数
     * @param bufferSizeMs 缓冲区大小（毫秒）
     * @return 是否成功
     */
    bool initialize(int sampleRate, int channels, int sampleSize, int bufferSizeMs);

    /**
     * @brief 入队音频数据
     * @param data 音频数据
     * @param timestamp 时间戳（毫秒）
     * @param timeoutMs 超时时间（毫秒），0表示不等待
     * @return 是否成功
     */
    bool enqueue(const QByteArray &data, qint64 timestamp, int timeoutMs = 0);

    /**
     * @brief 出队音频数据
     * @param buffer 输出缓冲区
     * @param maxSize 最大读取大小
     * @return 实际读取的字节数
     */
    qint64 dequeue(char *buffer, qint64 maxSize);

    /**
     * @brief 清空队列
     */
    void clear();

    /**
     * @brief 获取队列中的数据量（字节）
     */
    qint64 availableBytes() const;

    /**
     * @brief 获取队列中的数据量（毫秒）
     */
    qint64 availableMs() const;

    /**
     * @brief 统计信息
     */
    struct Statistics {
        int maxBuffers = 0;
        int currentBuffers = 0;
        qint64 totalBytes = 0;
        qint64 availableBytes = 0;
        qint64 availableMs = 0;
        int droppedBuffers = 0;
        double averageLatency = 0.0;
    };

    /**
     * @brief 获取统计信息
     */
    Statistics getStatistics() const;

private:
    /**
     * @brief 计算数据对应的时间长度（毫秒）
     */
    qint64 bytesToMs(qint64 bytes) const;

    /**
     * @brief 计算时间对应的数据大小（字节）
     */
    qint64 msToBytes(qint64 ms) const;

private:
    mutable QMutex mutex_;
    QWaitCondition bufferAvailable_;
    QWaitCondition spaceAvailable_;

    std::queue<AudioBufferItem> bufferQueue_;
    QByteArray currentBuffer_;    // 当前正在读取的缓冲区
    qint64 currentBufferPos_ = 0; // 当前缓冲区的读取位置

    // 配置参数
    int maxBuffers_;
    int sampleRate_ = 44100;
    int channels_ = 2;
    int sampleSize_ = 16;
    int bufferSizeMs_ = 100;
    qint64 bytesPerSecond_ = 0;

    // 统计信息
    mutable std::atomic<int> droppedBufferCount_{0};
    qint64 totalBytesEnqueued_ = 0;
    qint64 totalBytesDequeued_ = 0;

    // 时间统计
    QElapsedTimer timer_;
    bool initialized_ = false;
};

#endif // AUDIOBUFFERQUEUE_H