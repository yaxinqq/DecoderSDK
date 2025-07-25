#include "AudioBufferQueue.h"
#include <QDebug>
#include <QDateTime>
#include <algorithm>

AudioBufferQueue::AudioBufferQueue(int maxBuffers)
    : maxBuffers_(maxBuffers)
{
    timer_.start();
}

AudioBufferQueue::~AudioBufferQueue()
{
    clear();
}

bool AudioBufferQueue::initialize(int sampleRate, int channels, int sampleSize, int bufferSizeMs)
{
    QMutexLocker locker(&mutex_);
    
    sampleRate_ = sampleRate;
    channels_ = channels;
    sampleSize_ = sampleSize;
    bufferSizeMs_ = bufferSizeMs;
    
    // 计算每秒字节数
    bytesPerSecond_ = (sampleRate * channels * sampleSize) / 8;
    
    initialized_ = true;
    
    qDebug() << "[AudioBufferQueue] Initialized:"
             << "SampleRate:" << sampleRate_
             << "Channels:" << channels_
             << "SampleSize:" << sampleSize_
             << "BufferSizeMs:" << bufferSizeMs_
             << "BytesPerSecond:" << bytesPerSecond_;
    
    return true;
}

bool AudioBufferQueue::enqueue(const QByteArray &data, qint64 timestamp, int timeoutMs)
{
    if (!initialized_ || data.isEmpty()) {
        return false;
    }

    QMutexLocker locker(&mutex_);
    
    // 检查队列是否已满
    if (static_cast<int>(bufferQueue_.size()) >= maxBuffers_) {
        if (timeoutMs <= 0) {
            // 不等待，但在丢弃前尝试清理一个最老的缓冲区
            if (!bufferQueue_.empty()) {
                qWarning() << "[AudioBufferQueue] Queue full, removing oldest buffer to make space";
                bufferQueue_.pop();
                spaceAvailable_.wakeOne();
            } else {
                droppedBufferCount_.fetch_add(1);
                qWarning() << "[AudioBufferQueue] Queue full, dropping buffer";
                return false;
            }
        } else {
            // 等待空间可用
            if (!spaceAvailable_.wait(&mutex_, timeoutMs)) {
                droppedBufferCount_.fetch_add(1);
                return false;
            }
        }
    }
    
    // 添加到队列
    bufferQueue_.emplace(data, timestamp);
    totalBytesEnqueued_ += data.size();
    
    // 通知有数据可用
    bufferAvailable_.wakeOne();
    
    return true;
}

qint64 AudioBufferQueue::dequeue(char *buffer, qint64 maxSize)
{
    if (!initialized_ || !buffer || maxSize <= 0) {
        return 0;
    }

    QMutexLocker locker(&mutex_);
    
    qint64 totalRead = 0;
    
    while (totalRead < maxSize) {
        // 如果当前缓冲区已读完，获取下一个
        if (currentBuffer_.isEmpty() || currentBufferPos_ >= currentBuffer_.size()) {
            if (bufferQueue_.empty()) {
                // 没有更多数据
                break;
            }
            
            // 获取下一个缓冲区
            AudioBufferItem item = bufferQueue_.front();
            bufferQueue_.pop();
            
            currentBuffer_ = item.data;
            currentBufferPos_ = 0;
            
            // 通知有空间可用
            spaceAvailable_.wakeOne();
        }
        
        // 从当前缓冲区读取数据
        qint64 remainingInBuffer = currentBuffer_.size() - currentBufferPos_;
        qint64 remainingToRead = maxSize - totalRead;
        qint64 toRead = std::min(remainingInBuffer, remainingToRead);
        
        if (toRead > 0) {
            std::memcpy(buffer + totalRead, 
                       currentBuffer_.constData() + currentBufferPos_, 
                       toRead);
            
            currentBufferPos_ += toRead;
            totalRead += toRead;
            totalBytesDequeued_ += toRead;
        }
    }
    
    return totalRead;
}

void AudioBufferQueue::clear()
{
    QMutexLocker locker(&mutex_);
    
    // 清空队列
    while (!bufferQueue_.empty()) {
        bufferQueue_.pop();
    }
    
    // 清空当前缓冲区
    currentBuffer_.clear();
    currentBufferPos_ = 0;
    
    // 通知等待的线程
    spaceAvailable_.wakeAll();
    bufferAvailable_.wakeAll();
    
    qDebug() << "[AudioBufferQueue] Cleared";
}

qint64 AudioBufferQueue::availableBytes() const
{
    QMutexLocker locker(&mutex_);
    
    qint64 total = 0;
    
    // 计算队列中的数据
    std::queue<AudioBufferItem> tempQueue = bufferQueue_;
    while (!tempQueue.empty()) {
        total += tempQueue.front().data.size();
        tempQueue.pop();
    }
    
    // 加上当前缓冲区剩余的数据
    if (!currentBuffer_.isEmpty() && currentBufferPos_ < currentBuffer_.size()) {
        total += currentBuffer_.size() - currentBufferPos_;
    }
    
    return total;
}

qint64 AudioBufferQueue::availableMs() const
{
    return bytesToMs(availableBytes());
}

AudioBufferQueue::Statistics AudioBufferQueue::getStatistics() const
{
    QMutexLocker locker(&mutex_);
    
    Statistics stats;
    stats.maxBuffers = maxBuffers_;
    stats.currentBuffers = static_cast<int>(bufferQueue_.size());
    stats.totalBytes = totalBytesEnqueued_;
    stats.availableBytes = availableBytes();
    stats.availableMs = bytesToMs(stats.availableBytes);
    stats.droppedBuffers = droppedBufferCount_.load();
    
    // 计算平均延迟
    if (!bufferQueue_.empty()) {
        qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
        qint64 totalLatency = 0;
        std::queue<AudioBufferItem> tempQueue = bufferQueue_;
        int count = 0;
        
        while (!tempQueue.empty()) {
            const AudioBufferItem &item = tempQueue.front();
            totalLatency += currentTime - item.enqueueTime;
            tempQueue.pop();
            count++;
        }
        
        if (count > 0) {
            stats.averageLatency = static_cast<double>(totalLatency) / count;
        }
    }
    
    return stats;
}

qint64 AudioBufferQueue::bytesToMs(qint64 bytes) const
{
    if (bytesPerSecond_ <= 0) {
        return 0;
    }
    return (bytes * 1000) / bytesPerSecond_;
}

qint64 AudioBufferQueue::msToBytes(qint64 ms) const
{
    return (ms * bytesPerSecond_) / 1000;
}