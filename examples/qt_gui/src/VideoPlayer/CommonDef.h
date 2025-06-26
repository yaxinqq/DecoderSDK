#pragma once
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
} // namespace Stream