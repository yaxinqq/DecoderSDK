#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "Frame.h"

/**
 * @brief 帧队列类，用于缓存音视频帧
 */
class FrameQueue {
public:
    /**
     * @brief 构造函数
     * @param maxSize 最大帧数量
     * @param keepLast 是否保留最后一帧
     */
    FrameQueue(int maxSize = 3, bool keepLast = false);

    /**
     * @brief 析构函数
     */
    ~FrameQueue();

    // 禁用拷贝构造和拷贝赋值
    FrameQueue(const FrameQueue &) = delete;
    FrameQueue &operator=(const FrameQueue &) = delete;

    /**
     * @brief 推入一帧
     * @param frame 要推入的帧
     * @param timeout 超时时间(毫秒)，<0表示无限等待; 0立即返回;
     * >0表示等待指定时间
     * @return 成功返回true，失败返回false
     */
    bool push(Frame frame, int timeout = -1);

    /**
     * @brief 弹出一帧
     * @param frame 输出参数，存储弹出的帧
     * @param timeout 超时时间(毫秒)，<0表示无限等待; 0立即返回;
     * >0表示等待指定时间
     * @return 成功返回true，失败返回false
     */
    bool pop(Frame &frame, int timeout = -1);

    /**
     * @brief 非阻塞弹出一帧
     * @param frame 输出参数，存储弹出的帧
     * @return 成功返回true，失败返回false
     */
    bool tryPop(Frame &frame);

    /**
     * @brief 获取可写入的帧指针（用于直接写入）
     * @param timeout 超时时间(毫秒)
     * @return 可写入的帧指针，失败返回nullptr
     */
    Frame *getWritableFrame(int timeout = -1);

    /**
     * @brief 提交写入的帧
     * @return 成功返回true，失败返回false
     */
    bool commitFrame();

    // 查询接口
    bool empty() const;
    bool full() const;
    int size() const;
    int capacity() const;
    int remainingCount() const;

    // 控制接口
    void clear();
    void setAbortStatus(bool abort);
    void setSerial(int serial);
    void setKeepLast(bool keepLast);
    bool isKeepLast() const;

    // 设置最大容量(昂贵操作)
    bool setMaxCount(int maxCount);

private:
    // 推入一帧 内部使用
    bool pushInternal(Frame frame);
    // 弹出一帧 内部使用
    bool popInternal(Frame &frame);
    // 通知等待的线程
    void notifyWaiters();

    // 处理保留最后一帧的情况
    bool handleKeepLastCase(Frame &frame);
    // 是否能弹出
    bool canPop() const;
    // 应该弹出最后一帧
    bool shouldReturnLastFrame() const;
    // 等待数据
    bool waitForData(std::unique_lock<std::mutex> &lock, int timeout);

private:
    std::vector<Frame> queue_;  // 帧队列
    int head_;                  // 读取位置
    int tail_;                  // 写入位置
    int size_;                  // 当前大小
    int maxSize_;               // 最大容量
    bool keepLast_;             // 是否保留最后一帧

    int pendingWriteIndex_;  // 待写入帧的索引

    int serial_;                    // 当前版本序号
    std::atomic<bool> aborted_;     // 是否已中止
    mutable std::mutex mutex_;      // 互斥锁
    std::condition_variable cond_;  // 条件变量
};