/**
 * @file PcmPushBuffer.h
 * @brief 声明 Qt 推模式音频输出使用的 PCM 积压缓冲区。
 */

#ifndef PCM_PUSH_BUFFER_H
#define PCM_PUSH_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

/**
 * @brief 管理 QAudioSink/QAudioOutput 推模式下未能一次写入的 PCM 数据。
 *
 * QAudioSink/QAudioOutput 的设备缓冲区可能无法接收完整音频帧，write() 返回的字节数也可能
 * 小于请求大小。该类通过一个 FIFO 队列保存尚未写入设备的尾部数据，使 AudioRender 可以继续
 * 接收解码后的完整帧，并在后续 drain() 或 write() 调用中继续供给设备。
 *
 * 队列中的每个元素由调用方通过一次 write() 提交，因此当前使用方式约定每个元素对应一个
 * 完整音频帧。队列只保存原始 PCM 字节，不解析采样格式；超过容量上限时按元素丢弃最旧数据。
 *
 * 每个元素内部使用读偏移记录已写入部分，排空数据时无需从 vector 头部搬移剩余字节。
 * 本类不提供线程安全保证，调用方应保证同一个实例不会被多个线程并发访问。
 */
class PcmPushBuffer {
public:
    /**
     * @brief 一次写入或排空操作的结果统计。
     *
     * acceptedBytes 只统计本次操作期间真正写入底层设备的字节数，不包含本次刚刚放入队列、
     * 但尚未写入设备的数据。completedFrames 按完整队列元素计数；在当前调用约定下，一个
     * 队列元素对应一个音频帧。
     */
    struct WriteResult {
        // 本次操作中底层 write 回调实际接收的字节数。
        std::size_t acceptedBytes = 0;

        // 本次操作结束后仍在应用层队列中等待写入的字节数。
        std::size_t queuedBytes = 0;

        // 因容量上限而被丢弃的字节数。
        std::size_t droppedBytes = 0;

        // 因容量上限而被丢弃的完整队列元素数量。
        std::size_t droppedFrames = 0;

        // 本次操作中完整写入设备的队列元素数量。
        std::size_t completedFrames = 0;
    };

    /**
     * @brief 向底层音频设备写入 PCM 的回调。
     *
     * @param data PCM 数据起始地址。
     * @param size 本次请求写入的字节数。
     * @return 实际接收的字节数。
     *
     * 返回值会由 PcmPushBuffer 截断到 [0, size]。返回 0 或负数表示设备当前无法继续接收数据。
     */
    using WriteCallback =
        std::function<std::int64_t(const std::uint8_t *data, std::size_t size)>;

    /**
     * @brief 创建 PCM 积压缓冲区。
     *
     * @param maxPendingBytes 最大积压字节数；0 表示不限制。
     *
     * 容量上限按待写入 PCM 字节数计算，不包含底层设备自身缓冲区中的数据。
     */
    explicit PcmPushBuffer(std::size_t maxPendingBytes = 0);

    /**
     * @brief 写入一帧 PCM，并在存在积压时尝试继续排空队列。
     *
     * @param data PCM 数据起始地址。
     * @param size PCM 数据字节数。
     * @param writeCallback 底层设备写入回调。
     * @return 本次操作的接收、排队、丢弃和完成统计。
     *
     * 队列为空且回调能够完整接收时，数据不会复制到队列，直接写入设备。若只能部分接收，
     * 未接收的尾部数据会作为新元素入队。队列非空时，新数据会先按顺序入队，再从头排空已有
     * 数据，以保证设备收到的 PCM 顺序不变。
     *
     * data 为空、size 为 0 或 writeCallback 无效时直接返回空结果。
     */
    WriteResult write(const std::uint8_t *data, std::size_t size,
                      const WriteCallback &writeCallback);

    /**
     * @brief 按 FIFO 顺序将已积压的 PCM 数据写入设备。
     *
     * @param writeCallback 底层设备写入回调。
     * @return 本次排空操作的接收、完成和剩余积压统计。
     *
     * 回调每次只接收当前队首元素尚未写入的部分。回调返回 0 或负数时停止排空，保留当前
     * 元素及后续元素，等待下一次调用继续处理。
     */
    WriteResult drain(const WriteCallback &writeCallback);

    /**
     * @brief 设置最大积压字节数。
     *
     * @param maxPendingBytes 最大积压字节数；0 表示不限制。
     *
     * 修改不会立即裁剪现有队列，容量限制会在后续 enqueue() 时生效。
     */
    void setMaxPendingBytes(std::size_t maxPendingBytes) noexcept;

    /**
     * @brief 返回当前最大积压字节数；0 表示不限制。
     */
    std::size_t maxPendingBytes() const noexcept;

    /**
     * @brief 返回应用层队列中尚未写入设备的字节数。
     */
    std::size_t pendingBytes() const noexcept;

    /**
     * @brief 判断应用层队列是否为空。
     */
    bool empty() const noexcept;

    /**
     * @brief 返回扣除应用层积压后的有效设备空闲空间。
     *
     * @param sinkBytesFree 底层音频设备报告的当前空闲字节数。
     * @return max(0, sinkBytesFree - pendingBytes())。
     *
     * 该值用于避免仅根据设备空闲空间继续接收新帧，却忽略应用层已经积压的数据。
     */
    std::size_t effectiveBytesFree(std::size_t sinkBytesFree) const noexcept;

    /**
     * @brief 清空应用层积压数据。
     *
     * 该操作不会清空底层音频设备自身的缓冲区。
     */
    void clear() noexcept;

private:
    /**
     * @brief 队列中的一段连续 PCM 数据。
     */
    struct Chunk {
        // 拥有 PCM 数据的固定大小缓冲区。
        std::vector<std::uint8_t> data;

        // 已写入设备的数据偏移；剩余数据从该偏移开始。
        std::size_t offset = 0;

        /**
         * @brief 返回当前元素尚未写入设备的字节数。
         */
        std::size_t remaining() const noexcept
        {
            return data.size() - offset;
        }
    };

    /**
     * @brief 将一段完整 PCM 数据放入队列，并在必要时执行容量淘汰。
     */
    WriteResult enqueue(const std::uint8_t *data, std::size_t size);

    /**
     * @brief 丢弃并返回队首元素剩余的字节数。
     */
    std::size_t dropOldestChunk();

    // 按提交顺序保存待写入设备的 PCM 元素。
    std::deque<Chunk> chunks_;

    // 队列中所有元素尚未写入设备的字节总数。
    std::size_t pendingBytes_ = 0;

    // 允许积压的最大字节数；0 表示不限制。
    std::size_t maxPendingBytes_ = 0;
};

#endif // PCM_PUSH_BUFFER_H
