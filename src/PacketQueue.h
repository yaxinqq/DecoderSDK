#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "Packet.h"

/**
 * @brief 高性能数据包队列类，用于缓存音视频数据包
 * 支持多线程安全访问，提供阻塞和非阻塞操作
 */
class PacketQueue {
public:
    /**
     * @brief 构造函数
     * @param maxPacketCount 最大包数量，默认16
     * @param reserveSize 预分配大小，默认为maxPacketCount
     */
    explicit PacketQueue(int maxPacketCount = 16);

    /**
     * @brief 析构函数
     */
    ~PacketQueue();

    // 禁用拷贝构造和拷贝赋值
    PacketQueue(const PacketQueue &) = delete;
    PacketQueue &operator=(const PacketQueue &) = delete;

    /**
     * @brief 添加数据包
     * @param pkt 数据包
     * @param timeoutMs 超时时间：<0 无限阻塞，0 立即返回，>0 超时时间(ms)
     * @return 成功返回true，失败返回false
     */
    bool push(const Packet &pkt, int timeoutMs = 0);

    /**
     * @brief 弹出数据包
     * @param pkt 输出数据包
     * @param timeoutMs 超时时间：<0 无限阻塞，0 立即返回，>0 超时时间(ms)
     * @return 成功返回true，失败返回false
     */
    bool pop(Packet &pkt, int timeoutMs = 0);

    /**
     * @brief 尝试弹出数据包（非阻塞）
     * @param pkt 输出数据包
     * @return 成功返回true，队列为空返回false
     */
    bool tryPop(Packet &pkt);

    /**
     * @brief 查看队首数据包但不移除
     * @param pkt 输出数据包
     * @return 成功返回true，队列为空返回false
     */
    bool front(Packet &pkt) const;

    /**
     * @brief 刷新队列，清空所有数据包并增加序列号
     */
    void flush();

    /**
     * @brief 启动队列
     */
    void start();

    /**
     * @brief 中止队列，唤醒所有等待的线程
     */
    void abort();

    /**
     * @brief 检查队列是否已中止
     * @return 已中止返回true
     */
    bool isAborted() const noexcept;

    /**
     * @brief 获取队列包数量
     * @return 包数量
     */
    size_t packetCount() const noexcept;

    /**
     * @brief 获取队列包数据总大小
     * @return 数据总大小(字节)
     */
    size_t packetSize() const noexcept;

    /**
     * @brief 获取队列包总时长
     * @return 总时长(微秒)
     */
    int64_t packetDuration() const noexcept;

    /**
     * @brief 获取队列最大包数量
     * @return 最大包数量
     */
    size_t maxPacketCount() const noexcept;

    /**
     * @brief 获取队列序列号
     * @return 序列号
     */
    int serial() const noexcept;

    /**
     * @brief 检查队列是否已满
     * @return 已满返回true
     */
    bool isFull() const noexcept;

    /**
     * @brief 检查队列是否为空
     * @return 为空返回true
     */
    bool isEmpty() const noexcept;

    /**
     * @brief 设置队列最大包数量
     * @param maxCount 最大包数量，必须大于0
     */
    void setMaxPacketCount(size_t maxCount);

    /**
     * @brief 获取队列统计信息
     */
    struct Statistics {
        size_t count;
        size_t size;
        int64_t duration;
        int serial;
        bool aborted;
    };

    Statistics getStatistics() const;

private:
    // 内部辅助方法
    bool canPush() const noexcept;
    bool canPop() const noexcept;
    void updateStatisticsOnPush(const Packet &pkt) noexcept;
    void updateStatisticsOnPop(const Packet &pkt) noexcept;

private:
    // 使用deque提供更好的性能
    std::deque<Packet> queue_;

    // 队列状态
    std::atomic<bool> aborted_{false};
    std::atomic<int> serial_{0};
    std::atomic<size_t> maxPacketCount_;

    // 统计信息（原子操作保证线程安全）
    std::atomic<size_t> size_{0};
    std::atomic<int64_t> duration_{0};

    // 同步原语
    mutable std::mutex mutex_;
    std::condition_variable pushCond_;  // 用于push操作的条件变量
    std::condition_variable popCond_;   // 用于pop操作的条件变量
};