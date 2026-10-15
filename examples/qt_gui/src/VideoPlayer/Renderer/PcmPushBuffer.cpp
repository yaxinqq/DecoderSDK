/**
 * @file PcmPushBuffer.cpp
 * @brief 实现 Qt 推模式音频输出使用的 PCM 积压缓冲区。
 *
 * 本文件负责在设备只能部分接收音频帧时保留未写入的尾部数据，并在后续调用中按原始顺序
 * 继续供给设备。队列元素采用“提交即完整帧”的使用约定，容量淘汰也以元素为单位按帧执行。
 */

#include "PcmPushBuffer.h"

#include <algorithm>

/**
 * @brief 创建 PCM 积压缓冲区。
 *
 * @param maxPendingBytes 最大积压字节数；0 表示不限制。
 */
PcmPushBuffer::PcmPushBuffer(std::size_t maxPendingBytes)
    : maxPendingBytes_(maxPendingBytes)
{
}

/**
 * @brief 写入一帧 PCM，并在存在积压时尝试继续排空队列。
 *
 * 无积压时优先直接写入设备，避免完整帧也发生一次内存拷贝。设备只接收部分数据时，将未接收
 * 的尾部入队；已有积压时则先把新帧追加到队尾，再从队首排空，确保 PCM 的提交顺序不变。
 */
PcmPushBuffer::WriteResult PcmPushBuffer::write(const std::uint8_t *data, std::size_t size,
                                                const WriteCallback &writeCallback)
{
    WriteResult result;
    if (!data || size == 0 || !writeCallback) {
        return result;
    }

    // 快路径：没有历史积压时直接提交当前帧，避免为完整写入的数据复制缓冲区。
    if (chunks_.empty()) {
        const std::int64_t rawAccepted = writeCallback(data, size);

        // 底层回调可能返回异常值；统一限制在本次请求的有效范围内。
        result.acceptedBytes =
            static_cast<std::size_t>(std::clamp<std::int64_t>(rawAccepted, 0,
                                                              static_cast<std::int64_t>(size)));
        if (result.acceptedBytes == size) {
            result.completedFrames = 1;
        } else {
            // 仅保存设备尚未接收的尾部，已写入部分不再占用应用层内存。
            auto queued = enqueue(data + result.acceptedBytes, size - result.acceptedBytes);
            result.droppedBytes = queued.droppedBytes;
            result.droppedFrames = queued.droppedFrames;
        }
        result.queuedBytes = pendingBytes_;
        return result;
    }

    // 慢路径：已有积压时先保证新帧顺序入队，再尝试从队首继续写给设备。
    auto queued = enqueue(data, size);
    result.droppedBytes = queued.droppedBytes;
    result.droppedFrames = queued.droppedFrames;

    // 新入队的数据本轮可能尚未写设备，因此 acceptedBytes 只反映 drain() 实际接收的字节。
    const auto drained = drain(writeCallback);
    result.acceptedBytes = drained.acceptedBytes;
    result.completedFrames = drained.completedFrames;
    result.queuedBytes = pendingBytes_;
    return result;
}

/**
 * @brief 按 FIFO 顺序将已积压的 PCM 数据写入设备。
 *
 * 每次只处理队首元素，回调未接收完的部分通过 offset 保留。只有整个元素写完后才增加完成
 * 帧计数并移除元素；设备返回 0 或负数时立即停止，避免在这一轮中持续空转。
 */
PcmPushBuffer::WriteResult PcmPushBuffer::drain(const WriteCallback &writeCallback)
{
    WriteResult result;
    if (!writeCallback) {
        result.queuedBytes = pendingBytes_;
        return result;
    }

    while (!chunks_.empty()) {
        Chunk &chunk = chunks_.front();
        const std::uint8_t *data = chunk.data.data() + chunk.offset;
        const std::size_t available = chunk.remaining();
        const std::int64_t rawAccepted = writeCallback(data, available);

        // 将回调的异常返回值归一化，避免偏移和计数越过当前元素范围。
        const std::size_t accepted =
            static_cast<std::size_t>(std::clamp<std::int64_t>(rawAccepted, 0,
                                                              static_cast<std::int64_t>(available)));
        if (accepted == 0) {
            break;
        }

        result.acceptedBytes += accepted;
        chunk.offset += accepted;
        pendingBytes_ -= accepted;

        if (chunk.remaining() == 0) {
            ++result.completedFrames;
            chunks_.pop_front();
        }
    }

    result.queuedBytes = pendingBytes_;
    return result;
}

/**
 * @brief 设置最大积压字节数。
 *
 * 新上限只影响后续入队操作，不会在当前调用中裁剪已有队列。
 */
void PcmPushBuffer::setMaxPendingBytes(std::size_t maxPendingBytes) noexcept
{
    maxPendingBytes_ = maxPendingBytes;
}

/**
 * @brief 返回当前最大积压字节数。
 */
std::size_t PcmPushBuffer::maxPendingBytes() const noexcept
{
    return maxPendingBytes_;
}

/**
 * @brief 返回应用层队列中尚未写入设备的字节数。
 */
std::size_t PcmPushBuffer::pendingBytes() const noexcept
{
    return pendingBytes_;
}

/**
 * @brief 判断应用层队列是否为空。
 */
bool PcmPushBuffer::empty() const noexcept
{
    return chunks_.empty();
}

/**
 * @brief 返回扣除应用层积压后的有效设备空闲空间。
 *
 * 该结果用于让生产者基于应用层和设备层的总剩余空间判断是否还能继续提交新数据。
 */
std::size_t PcmPushBuffer::effectiveBytesFree(std::size_t sinkBytesFree) const noexcept
{
    return sinkBytesFree > pendingBytes_ ? sinkBytesFree - pendingBytes_ : 0;
}

/**
 * @brief 清空应用层积压数据。
 *
 * 仅释放队列自身拥有的 PCM 数据，不修改底层音频设备的状态或缓冲区。
 */
void PcmPushBuffer::clear() noexcept
{
    chunks_.clear();
    pendingBytes_ = 0;
}

/**
 * @brief 将一段完整 PCM 数据放入队列，并在必要时执行容量淘汰。
 *
 * 淘汰以完整元素为单位：先丢弃最旧的元素，直到能够容纳新数据。若新元素自身超过总容量，
 * 则直接丢弃新元素；若淘汰后仍无法容纳，则同样保留现有数据并丢弃新元素。
 */
PcmPushBuffer::WriteResult PcmPushBuffer::enqueue(const std::uint8_t *data, std::size_t size)
{
    WriteResult result;
    if (!data || size == 0) {
        return result;
    }

    // 单个元素本身超过总容量时无法通过淘汰旧数据解决，直接丢弃该元素。
    if (maxPendingBytes_ > 0 && size > maxPendingBytes_) {
        result.droppedBytes = size;
        result.droppedFrames = 1;
        return result;
    }

    // 按帧丢弃最旧的数据，直到队列有足够空间接收新元素。
    while (maxPendingBytes_ > 0 && pendingBytes_ > maxPendingBytes_ - size &&
           !chunks_.empty()) {
        result.droppedBytes += dropOldestChunk();
        ++result.droppedFrames;
    }

    // 如果现有元素无法提供足够空间，则保留旧数据并丢弃当前新元素。
    if (maxPendingBytes_ > 0 && size > maxPendingBytes_ - pendingBytes_) {
        result.droppedBytes += size;
        ++result.droppedFrames;
        return result;
    }

    // 拷贝数据以获得独立所有权，后续设备写入不再依赖调用方的帧缓冲区生命周期。
    chunks_.push_back({ std::vector<std::uint8_t>(data, data + size), 0 });
    pendingBytes_ += size;
    return result;
}

/**
 * @brief 丢弃并返回队首元素尚未写入设备的字节数。
 */
std::size_t PcmPushBuffer::dropOldestChunk()
{
    if (chunks_.empty()) {
        return 0;
    }

    const std::size_t droppedBytes = chunks_.front().remaining();
    pendingBytes_ -= droppedBytes;
    chunks_.pop_front();
    return droppedBytes;
}
