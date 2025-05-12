#ifndef DECODER_SDK_DEFINE_H
#define DECODER_SDK_DEFINE_H

#ifndef DECODER_SDK_NAMESPACE_BEGIN
#define DECODER_SDK_NAMESPACE_BEGIN namespace decoder_sdk {
#define DECODER_SDK_NAMESPACE_END }
#endif

#ifndef INTERNAL_NAMESPACE_BEGIN
#define INTERNAL_NAMESPACE_BEGIN namespace internal {
#define INTERNAL_NAMESPACE_END }
#endif

constexpr double kNoSyncThreshold = 10.0;

// 视频帧数据
typedef struct FrameData {
    int64_t pktPos;
} FrameData;

#endif // DECODER_SDK_DEFINE_H