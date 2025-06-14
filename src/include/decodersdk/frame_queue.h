#ifndef DECODER_SDK_FRAME_QUEUE_H
#define DECODER_SDK_FRAME_QUEUE_H
#include <memory>

#include "frame.h"
#include "sdk_global.h"

namespace decoder_sdk {
namespace internal {
class FrameQueue;
} // namespace internal
class FrameQueueImpl;

class DECODER_SDK_API FrameQueue {
public:
    /**
     * @brief 构造函数
     * @param queue 帧队列
     */
    FrameQueue(internal::FrameQueue *queue);

    /**
     * @brief 析构函数
     */
    ~FrameQueue();

    FrameQueue(const FrameQueue &);
    FrameQueue &operator=(const FrameQueue &);

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

private:
    std::unique_ptr<FrameQueueImpl> impl_;
};

} // namespace decoder_sdk

#endif // DECODER_SDK_FRAME_QUEUE_H