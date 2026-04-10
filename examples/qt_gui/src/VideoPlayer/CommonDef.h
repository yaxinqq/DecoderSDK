#pragma once
#include <QRectF>

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
} // namespace Stream