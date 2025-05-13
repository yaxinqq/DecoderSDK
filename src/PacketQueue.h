#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <functional>

extern "C" {
#include <libavcodec/avcodec.h>
}

#pragma region Packet
class Packet
{
public:
    Packet();
    explicit Packet(AVPacket *pkt);
    ~Packet();

    Packet(const Packet& other);
    Packet& operator=(const Packet& other);

    // 可以支持移动构造和移动赋值（提高效率）
    Packet(Packet&& other) noexcept;
    Packet& operator=(Packet&& other) noexcept;

    AVPacket* get() const;

    void setSerial(int serial);
    int serial() const;
private:
    AVPacket* packet_ = nullptr;
    int serial_;
};
#pragma endregion


#pragma region PacketQueue
/**
 * @brief 数据包队列类，用于缓存音视频数据包
 * 
 */
class PacketQueue {
public:    
    /**
     * @brief 构造函数
     */
    PacketQueue(int maxPacketCount = 3);

    /**
     * @brief 析构函数
     */
    ~PacketQueue();
    
    /*
     * @brief 添加数据包
     * @param pkt 数据包
     * @param delayTimeMs 阻塞时长 <0 无限阻塞，0 立即返回，>0 阻塞时长
     * @return 成功返回0，失败返回负值
    */
    bool push(const Packet& pkt, int delayTimeMs = 0);

    /*
     * @brief 弹出数据包
     * @param pkt 数据包
     * @param delayTimeMs 阻塞时长 <0 无限阻塞，0 立即返回，>0 阻塞时长
     * @return 成功返回0，失败返回负值
    */
    bool pop(Packet& pkt, int delayTimeMs = 0);

    /**
     * @brief 刷新队列
     * @param type 刷新类型
     */
    void flush();
    
     /**
     * @brief 开启队列
     */
    void start();

    /**
     * @brief 中止队列
     */
    void abort();    
    /**
     * @brief 是否已中止队列
     */
    bool isAbort() const;
    
    /**
     * @brief 获取队列包数量
     * @return 包数量
     */
    int packetCount() const;
    /**
     * @brief 获取队列包数据量
     * @return 数据量
     */
    int packetSize() const;
    /**
     * @brief 获取队列包总时长
     * @return 总时长
     */
    int64_t packetDuration() const;
    /**
     * @brief 获取队列包最大数量
     * @return 包最大数量
     */
    int maxPacketCount() const;
    /**
     * @brief 获取队列包的版本序号
     * @return 包版本序号
     */
    int serial() const;

    /*
     * @brief 销毁队列
    */
    void destory();
    /**
     * @brief 清空队列
     */
    void clear();

    /**
     * @brief 获取队列是否已满
     * @return 是否已满
     */
    bool isFull() const;
    /**
     * @brief 获取队列是否为空
     * @return 是否为空
     */
    bool isEmpty() const;

private:
    // 数据包队列
    std::queue<Packet> queue_;
    // 队列版本
    int serial_;

    // 请求停止
    std::atomic<bool> requestAborted_;
    // 队列最大包数量
    int maxPacketCount_;

    // 队列包总数据大小
    int size_;
    // 队列包总时长
    int64_t duration_;

    // 互斥锁
    mutable std::mutex mutex_;
    // 条件变量
    std::condition_variable cond_;
};
#pragma endregion