#include "include/decodersdk/common_define.h"
#include "version.h"

#ifdef MAGIC_ENUM_SUPPORTED
#include "magic_enum/magic_enum.hpp"
#endif

namespace decoder_sdk {
const char *getVersionString()
{
    return internal::getVersionString();
}

const char *getVersionStringFull()
{
    return internal::getVersionStringFull();
}

const char *getBuildInfo()
{
    return internal::getBuildInfo();
}

void getVersion(int *major, int *minor, int *patch, int *build)
{
    return internal::getVersion(major, minor, patch, build);
}

int checkVersion(int major, int minor, int patch)
{
    return internal::checkVersion(major, minor, patch);
}

std::vector<EventType> allEventTypes()
{
    std::vector<EventType> types;
#ifdef MAGIC_ENUM_SUPPORTED
    for (const auto &type : magic_enum::enum_values<EventType>()) {
        types.emplace_back(type);
    }
#else
    types = {
        EventType::kStreamOpened,         // 流已打开（调用open成功）
        EventType::kStreamClosed,         // 流已关闭（调用close成功）
        EventType::kStreamOpening,        // 流正在打开
        EventType::kStreamOpenFailed,     // 流打开失败
        EventType::kStreamClose,          // 关闭
        EventType::kStreamReadData,       // 读到第一帧数据
        EventType::kStreamReadError,      // 读取数据失败
        EventType::kStreamReadRecovery,   // 读取恢复
        EventType::kStreamEnded,          // 流结束
        EventType::kStreamLooped,         // 流循环播放
        EventType::kDecodeStarted,        // 解码已开始（调用startDecode成功）
        EventType::kDecodeStopped,        // 解码已停止（调用stopDecode成功）
        EventType::kDecodePaused,         // 解码已暂停（调用pauseDecode成功）
        EventType::kCreateDecoderSuccess, // 创建解码器成功
        EventType::kCreateDecoderFailed,  // 创建解码器失败
        EventType::kDestoryDecoder,       // 销毁解码器
        EventType::kDecodeFirstFrame,     // 解出第一帧数据
        EventType::kDecodeError,          // 解码错误
        EventType::kDecodeRecovery,       // 解码恢复
        EventType::kSeekStarted,          // 开始seek,
        EventType::kSeekSuccess,          // seek成功
        EventType::kSeekFailed,           // seek失败
        EventType::kRecordingStarted,     // 开始录制
        EventType::kRecordingStopped,     // 停止录制
        EventType::kRecordingError,       // 录制错误
    };
#endif

    return types;
}

std::string getEventTypeName(EventType type)
{
#ifdef MAGIC_ENUM_SUPPORTED
    if (!magic_enum::enum_contains(type)) {
        return {};
    }
    return std::string(magic_enum::enum_name(type));
#else
    switch (type) {
        case EventType::kStreamOpened:
            return "kStreamOpened";
        case EventType::kStreamClosed:
            return "kStreamClosed";
        case EventType::kStreamOpening:
            return "kStreamOpening";
        case EventType::kStreamOpenFailed:
            return "kStreamOpenFailed";
        case EventType::kStreamClose:
            return "kStreamClose";
        case EventType::kStreamReadData:
            return "kStreamReadData";
        case EventType::kStreamReadError:
            return "kStreamReadError";
        case EventType::kStreamReadRecovery:
            return "kStreamReadRecovery";
        case EventType::kStreamEnded:
            return "kStreamEnded";
        case EventType::kStreamLooped:
            return "kStreamLooped";
        case EventType::kDecodeStarted:
            return "kDecodeStarted";
        case EventType::kDecodeStopped:
            return "kDecodeStopped";
        case EventType::kDecodePaused:
            return "kDecodePaused";
        case EventType::kCreateDecoderSuccess:
            return "kCreateDecoderSuccess";
        case EventType::kCreateDecoderFailed:
            return "kCreateDecoderFailed";
        case EventType::kDestoryDecoder:
            return "kDestoryDecoder";
        case EventType::kDecodeFirstFrame:
            return "kDecodeFirstFrame";
        case EventType::kDecodeError:
            return "kDecodeError";
        case EventType::kDecodeRecovery:
            return "kDecodeRecovery";
        case EventType::kSeekStarted:
            return "kSeekStarted";
        case EventType::kSeekSuccess:
            return "kSeekSuccess";
        case EventType::kSeekFailed:
            return "kSeekFailed";
        case EventType::kRecordingStarted:
            return "kRecordingStarted";
        case EventType::kRecordingStopped:
            return "kRecordingStopped";
        case EventType::kRecordingError:
            return "kRecordingError";
        default:
            break;
    }
    return {};
#endif
}
} // namespace decoder_sdk