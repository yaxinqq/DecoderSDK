#ifndef DECODER_SDK_INTERNAL_SEEK_COORDINATOR_H
#define DECODER_SDK_INTERNAL_SEEK_COORDINATOR_H

#include "base/base_define.h"
#include <atomic>
#include <cstdint>
#include <mutex>

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

/**
 * @brief Seek 事务阶段
 */
enum class SeekPhase {
    kIdle,      ///< 空闲
    kRequested, ///< 已发起请求 (Controller -> Demuxer)
    kCommitted  ///< 已提交事务 (Demuxer 已完成底层 Seek 并切换 Serial)
};

/**
 * @brief Seek 事务状态
 */
struct SeekState {
    SeekPhase phase = SeekPhase::kIdle;
    double targetPosSec = -1.0;
    uint64_t videoSerial = 0;
    uint64_t audioSerial = 0;
    bool videoReached = false; ///< 视频是否已到达目标
    bool audioReached = false; ///< 音频是否已到达目标
};

/**
 * @brief Seek 事务协调器
 *
 * 集中管理异步 Seek 的全生命周期状态，确保各组件对 Seek 进度的认知一致。
 */
class SeekCoordinator {
public:
    SeekCoordinator();
    ~SeekCoordinator() = default;

    /**
     * @brief 发起 Seek 请求 (由 DecoderController 调用)
     * @param positionSec 目标位置 (秒)
     */
    void beginSeekRequest(double positionSec);

    /**
     * @brief 提交 Seek 事务 (由 Demuxer 调用)
     * @param positionSec 实际 Seek 的位置
     * @param videoSerial 新的视频数据序列号
     * @param audioSerial 新的音频数据序列号
     */
    void commitSeek(double positionSec, uint64_t videoSerial, uint64_t audioSerial);

    /**
     * @brief 宣告 Seek 失败 (由 Demuxer 调用)
     * @param positionSec 目标位置
     */
    void failSeek(double positionSec);

    /**
     * @brief 上报流已到达 Seek 目标位置 (由解码器调用)
     * @param isVideo 是否是视频流
     * @param serial 上报时的序列号
     */
    void reportReachedTarget(bool isVideo, uint64_t serial);

    /**
     * @brief 获取当前 Seek 状态快照
     */
    SeekState getState() const;

    /**
     * @brief 是否处于活跃 Seek 过程中 (Requested 或 Committed)
     */
    bool isSeekActive() const;

    /**
     * @brief 获取当前已提交的 Serial
     */
    uint64_t getCurrentVideoSerial() const;
    uint64_t getCurrentAudioSerial() const;

private:
    mutable std::mutex mutex_;
    SeekState state_;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_SEEK_COORDINATOR_H
