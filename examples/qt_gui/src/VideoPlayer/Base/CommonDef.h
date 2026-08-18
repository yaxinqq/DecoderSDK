#pragma once
#include <QRectF>
#include <QSize>

#include <cstdint>

// 视频流相关
namespace Stream {
enum class OpenMode : uint8_t {
    kExclusive, // 独占打开
    kReuse,     // 复用打开 （有录像需求时，不能使用复用模式，否则录制状态不好管理）
};

enum class PlayerState : uint8_t {
    Start,   // 开始播放
    Playing, // 正在播放
    Pause,   // 暂停播放
    Resume,  // 恢复播放（Pause=>playing之间的中间状态）
    Stop     // 停止播放
};

enum class AspectRatioMode : uint8_t {
    IgnoreAspectRatio, // 忽略宽高比
    KeepAspectRatio    // 保持宽高比
};

// 播放器流错误类型
enum class ErrorType : uint8_t {
    kOpenError,   // 打开错误
    kReadError,   // 读取错误
    kDecodeError, // 解码错误
    kRecordError, // 录制错误
};

// 协议
enum class Protocol : uint8_t {
    kWebSocket, // WS流
    kRtsp,      // RTSP流
    kNone
};

// 视频处理参数
struct VideoProcessParam {
    // 电子放大区域
    QRectF digitalZoomRect = QRectF(0.0, 0.0, 1.0, 1.0);

    // 是否水平翻转
    bool horizontalFlip = false;
    // 是否垂直翻转
    bool vecticalFlip = false;

    // 亮度
    float brightness = 0.0f;
    // 对比度
    float contrast = 1.0f;
    // 饱和度
    float saturation = 1.0f;
    // 色调
    float hue = 0.0f;
};

// 视频帧参数
struct VideoFrameParam {
    // 视频帧大小
    QSize size = {0, 0};

    // pts
    double pts = 0.0;

    VideoFrameParam() = default;
    VideoFrameParam(const QSize &size, double pts) : size{size}, pts{pts}
    {
    }
};

/**
 * @brief 主时钟源类型
 */
enum class ClockSourceType : uint8_t {
    kAudioMaster,    ///< 音频主时钟（默认，有音频流时推荐）
    kVideoMaster,    ///< 视频主时钟（纯视频文件流推荐）
    kExternalMaster, ///< 外部时钟（实时流或业务指定推荐）
    kNone            ///< 不启用时钟管理，由外部控制帧率（无脑渲染模式）
};

/**
 * @brief 调度动作
 */
enum class ScheduleAction : uint8_t {
    kNoClockReady, ///< 时钟尚未就绪，暂不处理
    kWait,         ///< 视频过早，需要等待
    kRenderNow,    ///< 视频正当时，立即渲染
    kDrop          ///< 视频过晚，应当丢弃
};

/**
 * @brief 播放同步配置参数
 */
struct PlaybackSyncOptions {
    double syncThresholdSec = 0.01; ///< 同步阈值（秒），在此范围内的帧被视为准时
    double dropThresholdSec = 0.05; ///< 丢帧阈值（秒），落后超过此值则丢弃
};

/**
 * @brief 视频调度决策结果
 */
struct VideoScheduleDecision {
    ScheduleAction action = ScheduleAction::kNoClockReady;
};

/**
 * @brief 音频调度决策结果
 */
struct AudioScheduleDecision {
    ScheduleAction action = ScheduleAction::kNoClockReady;
};
} // namespace Stream