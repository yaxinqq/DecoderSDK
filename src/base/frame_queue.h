#ifndef DECODER_SDK_INTERNAL_FRAME_QUEUE_H
#define DECODER_SDK_INTERNAL_FRAME_QUEUE_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "frame.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

/**
 * @brief 帧队列类，用于缓存音视频帧
 */
class FrameQueue : public std::enable_shared_from_this<FrameQueue> {
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
    bool push(const Frame &frame, int timeout = -1);

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
    /**
     * @brief 是否为空
     * @return true - 为空；false - 非空
     */
    bool empty() const;
    /**
     * @brief 是否已满
     * @return true - 已满；false - 未满
     */
    bool full() const;
    /**
     * @brief 获取当前队列大小
     * @return 队列大小
     */
    int size() const;
    /**
     * @brief 获取最大容量
     * @return 最大容量
     */
    int capacity() const;
    /**
     * @brief 获取剩余容量
     * @return 剩余容量
     */
    int remainingCount() const;

    // 控制接口
    /**
     * @brief 清除队列
     */
    void clear();
    /**
     * @brief 设置中止状态
     * @param abort true - 中止；false - 继续
     */
    void setAbortStatus(bool abort);
    /**
     * @brief 设置序号
     * @param serial 序号
     */
    void setSerial(int serial);
    /**
     * @brief 设置保留最后一帧
     * @param keepLast true - 保留；false - 不保留
     */
    void setKeepLast(bool keepLast);
    /**
     * @brief 获取保留最后一帧状态
     * @return true - 保留；false - 不保留
     */
    bool isKeepLast() const;

    /**
     * @brief 设置最大容量(昂贵操作)
     * @param maxCount 最大容量
     * @return 成功返回true，失败返回false
     */
    bool setMaxCount(int maxCount);

private:
    /**
     * @brief 推入一帧 内部使用
     * @param frame 要推入的帧
     * @return 成功返回true，失败返回false
     */
    bool pushInternal(const Frame &frame);
    /**
     * @brief 弹出一帧 内部使用
     * @param frame 输出参数，存储弹出的帧
     * @return 成功返回true，失败返回false
     */
    bool popInternal(Frame &frame);
    /**
     * @brief 通知等待的线程
     */
    void notifyWaiters();

    /**
     * @brief 处理保留最后一帧的情况
     * @param frame 输出参数，存储弹出的帧
     * @return 成功返回true，失败返回false
     */
    bool handleKeepLastCase(Frame &frame);
    /**
     * @brief 是否能弹出
     * @return true - 能弹出；false - 不能弹出
     */
    bool canPop() const;
    /**
     * @brief 应该弹出最后一帧
     * @return true - 应该弹出；false - 不应该弹出
     */
    bool shouldReturnLastFrame() const;
    /**
     * @brief 等待数据
     * @param lock 互斥锁
     * @param timeout 超时时间(毫秒)
     * @return 成功返回true，失败返回false
     */
    bool waitForData(std::unique_lock<std::mutex> &lock, int timeout);

private:
    std::vector<Frame> queue_; // 帧队列
    int head_;                 // 读取位置
    int tail_;                 // 写入位置
    int size_;                 // 当前大小
    int maxSize_;              // 最大容量
    bool keepLast_;            // 是否保留最后一帧

    int pendingWriteIndex_; // 待写入帧的索引

    int serial_;                   // 当前版本序号
    std::atomic<bool> aborted_;    // 是否已中止
    mutable std::mutex mutex_;     // 互斥锁
    std::condition_variable cond_; // 条件变量
};

DECODER_SDK_NAMESPACE_END
INTERNAL_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_FRAME_QUEUE_H