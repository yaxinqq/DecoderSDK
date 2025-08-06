#ifndef DECODER_SDK_COMMON_DEFINE_H
#define DECODER_SDK_COMMON_DEFINE_H
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sdk_global.h"

namespace decoder_sdk {
// ======================== 通用 ======================== //
#pragma region normal

// 媒体类型枚举
enum class MediaType : uint8_t {
    kMediaTypeUnknown = 0, // 未知
    kMediaTypeVideo,       // 视频
    kMediaTypeAudio,       // 音频
};

// 硬件加速类型
enum class HWAccelType : uint8_t {
    kNone,        // 不使用硬件加速
    kAuto,        // 自动选择最佳硬件加速
    kDxva2,       // DirectX Video Acceleration 2.0
    kD3d11va,     // Direct3D 11 Video Acceleration
    kCuda,        // NVIDIA CUDA
    kVaapi,       // Video Acceleration API (Linux)
    kVdpau,       // Video Decode and Presentation API for Unix (Linux)
    kQsv,         // Intel Quick Sync Video
    kVideoToolBox // Apple VideoToolbox (macOS/iOS)
};

// 图像格式枚举（部分）
enum class ImageFormat : uint8_t {
    // 软解
    kNV12,    // NV12格式
    kNV21,    // NV21格式
    kYUV420P, // YUV420P格式
    kYUV422P, // YUV422P格式
    kYUV444P, // YUV444P格式
    kRGB24,   // RGB24格式
    kBGR24,   // BGR24格式
    kRGBA,    // RGBA格式
    kBGRA,    // BGRA格式

    // 硬解
    kDxva2,        // DXVA表面格式，仅Windows
    kD3d11va,      // Direct3D 11 纹理指针
    kCuda,         // CUDA内存句柄
    kVaapi,        // VA表明，使用DRM/VA display
    kVdpau,        // VDPAU表面格式
    kQsv,          // Intel QSV surface
    kVideoToolBox, // Apple平台Surface句柄

    kUnknown, // 未知
};

// 音频采样格式枚举
enum class AudioSampleFormat : uint8_t {
    kFmtU8,  ///< unsigned 8 bits
    kFmtS16, ///< signed 16 bits
    kFmtS32, ///< signed 32 bits
    kFmtFlt, ///< float
    kFmtDbl, ///< double

    kFmtU8P,  ///< unsigned 8 bits, planar
    kFmtS16P, ///< signed 16 bits, planar
    kFmtS32P, ///< signed 32 bits, planar
    kFmtFltP, ///< float, planar
    kFmtDblP, ///< double, planar
    kFmtS64,  ///< signed 64 bits
    kFmtS64P, ///< signed 64 bits, planar

    kUnknown, // 未知
};

/**
 * @brief 获取SDK版本字符串
 * @return 版本字符串，格式为"major.minor.patch"
 */
DECODER_SDK_API const char *getVersionString();

/**
 * @brief 获取SDK完整版本字符串
 * @return 完整版本字符串，格式为"major.minor.patch.build"
 */
DECODER_SDK_API const char *getVersionStringFull();

/**
 * @brief 获取SDK构建信息
 * @return 构建信息字符串
 */
DECODER_SDK_API const char *getBuildInfo();

/**
 * @brief 获取SDK版本号
 * @param major 主版本号输出
 * @param minor 次版本号输出
 * @param patch 补丁版本号输出
 * @param build 构建版本号输出
 */
void DECODER_SDK_API getVersion(int *major, int *minor, int *patch, int *build);

/**
 * @brief 检查版本兼容性
 * @param major 要求的主版本号
 * @param minor 要求的次版本号
 * @param patch 要求的补丁版本号
 * @return 如果当前版本兼容返回1，否则返回0
 */
int DECODER_SDK_API checkVersion(int major, int minor, int patch);
// ===================================================== //

// ====================== 事件系统 ====================== //
#pragma region event_system

// 事件类型枚举
enum class EventType : uint32_t {
    // 流事件
    kStreamOpened = 1,   // 流已打开（调用open成功）
    kStreamClosed,       // 流已关闭（调用close成功）
    kStreamOpening,      // 流正在打开
    kStreamOpenFailed,   // 流打开失败
    kStreamClose,        // 关闭
    kStreamReadData,     // 读到第一帧数据
    kStreamReadError,    // 读取数据失败
    kStreamReadRecovery, // 读取恢复
    kStreamEnded,        // 流结束
    kStreamLooped,       // 流循环播放（新增）

    // 解码相关事件
    kDecodeStarted = 20,   // 解码已开始
    kDecodeStopped,        // 解码已停止
    kDecodePaused,         // 解码已暂停
    kCreateDecoderSuccess, // 创建解码器成功
    kCreateDecoderFailed,  // 创建解码器失败
    kDestoryDecoder,       // 销毁解码器
    kDecodeFirstFrame,     // 解出第一帧数据
    kDecodeError,          // 解码错误
    kDecodeRecovery,       // 解码恢复

    // seek相关事件
    kSeekStarted = 40, // 开始seek,
    kSeekSuccess,      // seek成功
    kSeekFailed,       // seek失败

    // 录制相关事件
    kRecordingStarted = 60, // 开始录制
    kRecordingStopped,      // 停止录制
    kRecordingError,        // 录制错误
};
/**
 * @brief 获取所有事件类型
 * @return std::vector<EventType> 所有事件类型
 */
std::vector<EventType> DECODER_SDK_API allEventTypes();
/**
 * @brief 获取事件名称
 * @param type 事件类型
 * @return std::string 事件名称
 */
std::string DECODER_SDK_API getEventTypeName(EventType type);

// 事件参数基类
struct EventArgs {
public:
    EventArgs(const std::string &source = "", const std::string &description = "", int errcode = 0,
              const std::string &errorMessage = "")
        : source(source), description(description), errorCode(errcode), errorMessage(errorMessage)
    {
    }
    virtual ~EventArgs() = default;

    // 事件时间戳
    std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::now();

    // 事件源标识
    std::string source;
    // 事件描述
    std::string description;
    // 错误码，0表示无错误
    int errorCode = 0;
    // 错误信息
    std::string errorMessage;
};

// 流事件参数
struct StreamEventArgs : public EventArgs {
public:
    StreamEventArgs(const std::string &filePath = "", const std::string &source = "",
                    const std::string &description = "", int errcode = 0,
                    const std::string &errorMessage = "")
        : EventArgs(source, description, errcode, errorMessage), filePath(filePath)
    {
    }

    std::string filePath; // 文件路径

    std::optional<int> totalTime; // 文件总时长(s)，只会在kStreamOpened事件中携带，但也有可能为空
};

// 解码器事件参数
struct DecoderEventArgs : public EventArgs {
public:
    DecoderEventArgs(const std::string &codecName = "", int streamIndex = -1,
                     MediaType mediaType = MediaType::kMediaTypeUnknown,
                     bool isHardwareAccel = false, const std::string &source = "",
                     const std::string &description = "", int errcode = 0,
                     const std::string &errorMessage = "")
        : EventArgs(source, description, errcode, errorMessage),
          codecName(codecName),
          streamIndex(streamIndex),
          mediaType(mediaType),
          isHardwareAccel(isHardwareAccel)
    {
    }

    std::string codecName;        // 编解码器名称
    int streamIndex;              // 流索引
    MediaType mediaType;          // 媒体类型
    bool isHardwareAccel = false; // 是否硬件加速
};

// Seek事件参数
struct SeekEventArgs : public EventArgs {
public:
    SeekEventArgs(double position = 0.0, double targetPosition = 0.0,
                  const std::string &source = "", const std::string &description = "",
                  int errcode = 0, const std::string &errorMessage = "")
        : EventArgs(source, description, errcode, errorMessage),
          position(position),
          targetPosition(targetPosition)
    {
    }

    double position;       // 当前位置（秒）
    double targetPosition; // 目标位置（秒）
};

// 录制事件参数
struct RecordingEventArgs : public EventArgs {
public:
    RecordingEventArgs(const std::string &outputPath = "", const std::string &format = "",
                       const std::string &source = "", const std::string &description = "",
                       int errcode = 0, const std::string &errorMessage = "")
        : EventArgs(source, description, errcode, errorMessage),
          outputPath(outputPath),
          format(format)
    {
    }

    std::string outputPath; // 输出文件路径
    std::string format;     // 录制格式
};

// 循环播放模式枚举（新增）
enum class LoopMode : uint8_t {
    kNone = 0, // 不循环
    kSingle,   // 单次循环
    kInfinite  // 无限循环
};

// 循环播放事件参数（新增）
struct LoopEventArgs : public EventArgs {
public:
    LoopEventArgs(int currentLoop = 0, int maxLoops = 0, const std::string &source = "",
                  const std::string &description = "", int errcode = 0,
                  const std::string &errorMessage = "")
        : EventArgs(source, description, errcode, errorMessage),
          currentLoop(currentLoop),
          maxLoops(maxLoops)
    {
    }

    int currentLoop; // 当前循环次数
    int maxLoops;    // 最大循环次数（-1表示无限循环）
};

// 连接类型
enum class ConnectionType : uint8_t {
    kDirect, // 同步调用
    kQueued, // 异步队列
    kAuto    // 自动选择
};

// 事件回调类型
using EventCallback = void(EventType, std::shared_ptr<EventArgs>);

// 监听器句柄
using EventListenerHandle = uint64_t;
using GlobalEventListenerHandle = std::unordered_map<EventType, EventListenerHandle>;

#pragma endregion
// ===================================================== //

// ====================== 解码相关 ====================== //
#pragma region decoder

// 解码器性能指标结构
struct DecoderStatistics {
    // 解码的帧数
    std::atomic<uint64_t> framesDecoded{0};
    // 错误统计 AVERROR_EOF 和 AVERROR(EAGAIN) 不计算在内
    std::atomic<uint64_t> errorsCount{0};
    // 解码时间统计 ms
    std::atomic<uint64_t> totalDecodeTime{0};
    // 连续错误次数
    std::atomic<uint64_t> consecutiveErrors{0};
    // 开始时间
    std::chrono::steady_clock::time_point startTime;

    /**
     * @brief 重置统计数据
     */
    void reset()
    {
        framesDecoded.store(0);
        errorsCount.store(0);
        totalDecodeTime.store(0);
        consecutiveErrors.store(0);
        startTime = std::chrono::steady_clock::now();
    }

    /**
     * @brief 获取平均帧率
     * @return double
     */
    double getFrameRate() const
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - startTime)
                                 .count();
        return elapsed > 0 ? static_cast<double>(framesDecoded.load()) / elapsed : 0.0;
    }
};

// 硬件加速信息结构体
struct HWAccelInfo {
    HWAccelType type;                   // 硬件设备类型
    std::string name;                   // 名称
    std::string description;            // 描述
    bool available;                     // 是否可用
    ImageFormat hwFormat;               // 硬件像素格式
    std::vector<ImageFormat> swFormats; // 支持的软件像素格式
};
#pragma endregion
// ===================================================== //

// ====================== 同步相关 ====================== //
#pragma region sync

// 时钟同步选项
enum class MasterClock : uint8_t {
    kAudio,   // 音频时钟
    kVideo,   // 视频时钟
    kExternal // 外部时钟
};

// 同步状态
enum class SyncState {
    kInSync,      // 同步良好
    kSlightDrift, // 轻微漂移
    kOutOfSync    // 严重失步
};

// 同步统计信息
struct SyncStats {
    SyncState state;      // 同步状态
    double videoDrift;    // 视频漂移（秒）
    double audioDrift;    // 音频漂移（秒）
    double masterClock;   // 主时钟（秒）
    int droppedFrames;    // 丢帧数量
    int duplicatedFrames; // 重复帧数量
    double avgDelay;      // 平均延迟（秒）
};

// 同步质量统计结构
struct SyncQualityStats {
    int totalSyncCount;  // 总同步次数
    int goodSyncCount;   // 同步良好次数
    int poorSyncCount;   // 同步失步次数
    double goodSyncRate; // 百分比
    double avgDrift;     // 平均漂移（秒）
    double maxDrift;     // 最大漂移（秒）
};
#pragma endregion
// ===================================================== //

// ====================== 使用相关 ====================== //
#pragma region Controller

// 解码器配置
// 创建硬件解码器上下文的回调
using CreateHWContextCallback = std::function<void *(HWAccelType type)>;
// 释放硬件解码器上下文的回调 会在AVHWDeviceContext的free回调中执行。
// userHwContext 是 CreateHWContextCallback 生成的硬件解码器上下文
using FreeHWContextCallback = std::function<void(HWAccelType type, void *userHwContext)>;
struct Config {
    // 解码的媒体类型
    enum DecodeMediaType : uint8_t {
        kVideo = 1, // 视频
        kAudio = 2, // 音频

        kAll = kVideo | kAudio, // 所有
    };

    // 是否开启帧率控制
    bool enableFrameRateControl = true;
    // 播放速度
    double speed = 1.0;
    // 硬件解码器类型
    HWAccelType hwAccelType = HWAccelType::kAuto;
    // 硬件解码器设备索引
    int hwDeviceIndex = 0;
    // 软解时的视频输出格式
    ImageFormat swVideoOutFormat = ImageFormat::kYUV420P;
    // 需要解码后的帧位于内存中
    bool requireFrameInSystemMemory = false;

    // 需要解码的媒体类型（如果有视频+音频，但只想解某一类型的媒体数据，建议设置该参数）
    // 否则可能会因为另外类型媒体数据的PackQueue满队，导致程序阻塞
    DecodeMediaType decodeMediaType = DecodeMediaType::kAll;

    // 硬件解码自动退化到软解的配置，是否启用硬件解码失败时自动退化到软解
    bool enableHardwareFallback = true;

    // 硬件上下文创建回调。
    // 每次创建解码器时，会调用一次回调，用于获取当前所选解码类型对应的硬件上下文。
    // 若希望多个解码器共用同一个上下文，请自行缓存并复用上下文指针。上下文指针由上层管理。
    // 如果未传入回调或是回调返回空指针，则由解码库自行创建。
    // 目前只建议D3D11和DXVA2的硬件上下文由用户自己管理。其它情况由库内部接管
    CreateHWContextCallback createHwContextCallback = nullptr;
    // 硬件上下文销毁回调。
    // 每次释放解码器时，会调用一次回调，用于释放当前所选解码类型对应的硬件上下文。
    FreeHWContextCallback freeHwContextCallback = nullptr;

    // 重连配置
    bool enableAutoReconnect = true; // 是否启用自动重连
    int maxReconnectAttempts = 5;    // 最大重连次数（-1表示无限重连）
    int reconnectIntervalMs = 3000;  // 重连间隔(毫秒)，建议3秒

    // 预缓冲配置
    struct PreBufferConfig {
        // 是否启用预缓冲
        bool enablePreBuffer = false;
        // 视频预缓冲帧数
        int videoPreBufferFrames = 0;
        // 音频预缓冲包数
        int audioPreBufferPackets = 0;
        // 是否需要音视频都达到预缓冲才开始
        bool requireBothStreams = false;
        // 预缓冲完成后是否自动开始解码
        bool autoStartAfterPreBuffer = true;
    } preBufferConfig;

    // 音频采样格式是否交错
    bool audioInterleaved = true;
};

// 预缓冲状态
enum class PreBufferState {
    kDisabled,      // 未启用预缓冲
    kWaitingBuffer, // 等待预缓冲完成
    kReady,         // 预缓冲完成，正在解码
};

// 预缓冲进度结构
struct PreBufferProgress {
    size_t videoBufferedFrames;  // 已缓冲视频帧数
    size_t audioBufferedPackets; // 已缓冲音频包数
    size_t videoRequiredFrames;  // 所需视频缓冲帧数
    size_t audioRequiredPackets; // 所需音频缓冲包数
    double videoProgressPercent; // 视频缓冲百分比 0.0-1.0
    double audioProgressPercent; // 音频缓冲百分比 0.0-1.0
    bool isVideoReady;           // 视频缓冲是否就绪
    bool isAudioReady;           // 音频缓冲是否就绪
    bool isOverallReady;         // 整体缓冲是否就绪
};

// 异步打开操作的结果状态
enum class AsyncOpenResult {
    kSuccess,  // 成功
    kFailed,   // 失败
    kCancelled // 被取消
};
// 异步打开完成的回调函数类型
using AsyncOpenCallback =
    std::function<void(AsyncOpenResult result, bool openSuccess, const std::string &errorMessage)>;
#pragma endregion
// ===================================================== //
} // namespace decoder_sdk

#endif // DECODER_SDK_COMMON_DEFINE_H