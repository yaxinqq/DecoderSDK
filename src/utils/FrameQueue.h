#ifndef DECODER_SDK_FRAMEQUEUE_H
#define DECODER_SDK_FRANEQUEUE_H
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <functional>

extern "C"
{
#include <libavutil/frame.h>
}

#include "base/define.h"
#include "utils/PacketQueue.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

#pragma region Frame
class Frame
{
public:
    Frame();
    explicit Frame(AVFrame *srcFrame);
    Frame(const Frame &other);
    Frame &operator=(const Frame &other);
    ~Frame();

    // 获得帧指针
    AVFrame *get() const;
    
    // 获得序列号
    int serial() const;
    // 设置序列号
    void setSerial(int serial);

    // 获得帧时长
    double duration() const;
    // 设置帧时长
    void setDuration(double duration);

    // 是否是硬解码
    bool isInHardware() const;
    // 设置是否是硬解码
    void setIsInHardware(bool isInHardware);

    // 是否需要翻转
    bool isFlipV() const;
    // 设置是否需要翻转
    void setIsFlipV(bool isFlipV);

private:
    void release();
    void unref();

#ifdef USE_VAAPI
    bool copyFrmae(AVFrame *srcFrame);
#endif

private:
    friend class FrameQueue;

    // 帧
    AVFrame *frame_ = nullptr;
    // 序列号
    int serial_;
    // 帧的时长
    double duration_;
    // 是否是硬解码
    bool isInHardware_;
    // 是否需要翻转
    bool isFlipV_;
};
#pragma endregion

#pragma region FrameQueue
/**
 * @brief 数据包队列类，用于缓存音视频数据包
 *
 */
class FrameQueue
{
public:
    /**
     * @brief 构造函数
     * @param maxSize 最大帧数量
     * @param keepLast 是否保留最后一帧
     */
    FrameQueue(int maxSize = 3, bool keepLast = true);

    /**
     * @brief 析构函数
     */
    ~FrameQueue();

    /**
     * @brief 设置是否需要终止，和对应的PacketQueue保持一致
     * @param abort 是否终止
     */
    void setAbortStatus(bool abort);
    /**
     * @brief 设置帧队列版本序号
     * @param serial 版本序号
     */
    void setSerial(int serial);

    /*
     * @brief 唤醒条件变量
    */
    void awakeCond();

    /**
     * @brief 获取可写入的帧
     * @return 可写入的帧指针，失败返回nullptr
     */
    Frame *peekWritable();
    /**
     * @brief 获取可读取的帧
     * @return 可读取的帧指针，失败返回nullptr
     */
    Frame *peekReadable();

    /**
     * @brief 获取可读取的帧
     * @return 可读取的帧指针，失败返回nullptr
     */
    Frame *peek();

    /**
     * @brief 获取下一帧
     * @return 下一帧指针，失败返回nullptr
     */
    Frame *peekNext();

    /**
     * @brief 获取最后一帧
     * @return 最后一帧指针，失败返回nullptr
     */
    Frame *peekLast();

    /**
     * @brief 推入一帧
     * @return 成功返回0，失败返回负值
     */
    int push();

    /**
     * @brief 弹出一帧
     * @return 成功返回0，失败返回负值
     */
    int pop();

    /*
     * @brief 移动到下一帧，更新 FrameQueue 的读取位置（rindex），并释放当前帧资源。
    */
   void next();

    /**
     * @brief 获取并弹出一帧
     * @param timeout 超时时间(毫秒)，<0表示无限等待; 0立即返回; >0表示等待指定时间
     * @return 失败返回false
     */
    bool popFrame(Frame &frame, int timeout = -1);

    /**
     * @brief 目前还有多少帧没有显示
     * @return 返回尚未显示的帧数量
     */
    int remainingCount() const;

    /**
     * @brief 最后一帧的播放时间戳
     * @return 返回最后一帧的播放时间戳
     */
    int lastFramePts();



private:
    std::vector<Frame> queue_;     // 帧队列
    int rindex_;                   // 读索引
    int rindexShown_;              // 当前帧是否已展示
    int windex_;                   // 写索引
    int size_;                     // 队列大小
    int maxSize_;                  // 最大帧数量
    bool keepLast_;                // 是否保留最后一帧

    bool serial_;                  // 当前版本，和packetQueue一致
    bool aborted_;                 // 队列是否已中止，和packetQueue一致
    std::mutex mutex_;             // 互斥锁
    std::condition_variable cond_; // 条件变量
};
#pragma endregion

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_FRAMEQUEUE_H