#include "hardware_accel.h"

#include <algorithm>

#include "logger/logger.h"
#include "utils/common_utils.h"
#include "version.h"

#ifdef D3D11VA_AVAILABLE
#include <d3d11.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#endif

#ifdef DXVA2_AVAILABLE
#include <d3d9.h>
#include <dxva2api.h>
extern "C" {
#include <libavutil/hwcontext_dxva2.h>
}
#endif

#ifdef CUDA_AVAILABLE
extern "C" {
#include <libavutil/hwcontext_cuda.h>
}
#endif

#ifdef VAAPI_AVAILABLE
extern "C" {
#include <libavutil/hwcontext_vaapi.h>
}
#endif

#ifdef VULKAN_AVAILABLE
#include "hardware_accel_vulkan_helper.h"
#endif

#ifdef QSV_AVAILABLE
#include <libavutil/hwcontext_qsv.h>
#endif

#ifdef AMF_AVAILABLE
#include <libavutil/hwcontext_amf.h>
#endif

namespace {
struct FreeHWContext {
    decoder_sdk::HWAccelType type;
    void *userHwContext;
    decoder_sdk::FreeHWContextCallback callback;
};

// 硬件解码器上下文释放回调
void freeHWContextCallback(struct AVHWDeviceContext *ctx)
{
    if (!ctx || !ctx->user_opaque)
        return;

    // 从user_opaque中获取FreeHWContext结构
    auto *freeContext = static_cast<FreeHWContext *>(ctx->user_opaque);

    // 如果有用户提供的释放回调，则调用它
    if (freeContext && freeContext->callback) {
        freeContext->callback(freeContext->type, freeContext->userHwContext);
    }

    // 释放FreeHWContext结构
    delete freeContext;
    ctx->user_opaque = nullptr;
}

} // namespace

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

// 静态成员初始化
std::map<AVCodecContext *, HardwareAccel *> HardwareAccel::hwAccelMap_;
std::mutex HardwareAccel::hwAccelMapMutex_;

//-----------------------------------------------------------------------------
// HardwareAccel 实现
//-----------------------------------------------------------------------------

HardwareAccel::HardwareAccel()
    : type_(HWAccelType::kNone),
      hwDeviceCtx_(nullptr),
      hwPixFmt_(AV_PIX_FMT_NONE),
      initialized_(false),
      isUserContext_(false),
      deviceIndex_(0),
      hwChildDeviceCtx_(nullptr)
{
}

HardwareAccel::~HardwareAccel()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 从映射表中移除所有引用此对象的条目
    {
        std::lock_guard<std::mutex> mapLock(hwAccelMapMutex_);
        for (auto it = hwAccelMap_.begin(); it != hwAccelMap_.end();) {
            if (it->second == this) {
                it = hwAccelMap_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 释放硬件设备上下文（包括本体 + 派生）
    clearHwCtx();

    initialized_ = false;
}

bool HardwareAccel::init(HWAccelType type, HWAccelType backendType, int deviceIndex,
                         const CreateHWContextCallback &createCallback,
                         const FreeHWContextCallback &freeCallback)

{
    std::lock_guard<std::mutex> lock(mutex_);

    // 如果已经初始化，先释放资源
    clearHwCtx();

    initialized_ = false;
    deviceIndex_ = deviceIndex;

    // 如果类型为NONE，直接返回成功
    if (type == HWAccelType::kNone) {
        return true;
    }

    // 确定硬件设备类型
    AVHWDeviceType deviceType;
    if (type == HWAccelType::kAuto) {
        deviceType = findBestHWAccelType();
        if (deviceType == AV_HWDEVICE_TYPE_NONE) {
            LOG_WARN("No suitable hardware acceleration method found");
            type_ = HWAccelType::kNone;
            return false;
        }
        type = fromAVHWDeviceType(deviceType);
    } else {
        deviceType = toAVHWDeviceType(type);
        if (!isAvailableHWAccelType(type)) {
            // 回退到软解
            type_ = HWAccelType::kNone;
            return true;
        }
    }

    // 初始化硬件设备
#if defined(OS_LINUX)
    AVHWDeviceType backendDeviceType = AV_HWDEVICE_TYPE_VAAPI;
#else
    AVHWDeviceType backendDeviceType = toAVHWDeviceType(backendType);
#endif
    if (!initHWDevice(deviceType, backendDeviceType, deviceIndex_, createCallback, freeCallback)) {
        LOG_WARN("Failed to initialize hardware device: {}", getHWAccelTypeName(type));
        type_ = HWAccelType::kNone;
        return false;
    }

    // 获得硬件设备类型
    type_ = fromAVHWDeviceType(deviceType);
    // 获取硬件像素格式
    hwPixFmt_ = getHWPixelFormatForDevice(deviceType);
    if (hwPixFmt_ == AV_PIX_FMT_NONE) {
        LOG_WARN("Failed to get hardware pixel format for device: {}", getHWAccelTypeName(type_));
        clearHwCtx();
        return false;
    }

    initialized_ = true;
    return true;
}

bool HardwareAccel::setupDecoder(AVCodecContext *codecCtx)
{
    if (!codecCtx || !initialized_ || !hwDeviceCtx_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 设置硬件加速上下文
    codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
    if (!codecCtx->hw_device_ctx) {
        LOG_WARN("Failed to reference hardware device context");
        return false;
    }

    // 设置获取硬件帧的回调函数
    codecCtx->get_format = getHWPixelFormat;

    // 将此对象与解码器上下文关联
    {
        std::lock_guard<std::mutex> mapLock(hwAccelMapMutex_);
        hwAccelMap_[codecCtx] = this;
    }

    return true;
}

AVFrame *HardwareAccel::getHWFrame(AVFrame *frame)
{
    if (!frame || !initialized_) {
        return nullptr;
    }

    // 如果已经是硬件帧，直接返回
    if (frame->format == hwPixFmt_) {
        return frame;
    }

    // 创建硬件帧
    AVFrame *hwFrame = av_frame_alloc();
    if (!hwFrame) {
        LOG_WARN("Failed to allocate hardware frame");
        return nullptr;
    }

    // 设置硬件帧参数
    hwFrame->width = frame->width;
    hwFrame->height = frame->height;
    hwFrame->format = hwPixFmt_;

    // 分配硬件帧缓冲区
    int ret = av_hwframe_get_buffer(hwDeviceCtx_, hwFrame, 0);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_WARN("Failed to allocate hardware frame buffer: {}", errBuf);
        av_frame_free(&hwFrame);
        return nullptr;
    }

    // 将软件帧数据传输到硬件帧
    ret = av_hwframe_transfer_data(hwFrame, frame, 0);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_WARN("Failed to transfer frame data to hardware: {}", errBuf);
        av_frame_free(&hwFrame);
        return nullptr;
    }

    // 复制帧属性
    av_frame_copy_props(hwFrame, frame);

    return hwFrame;
}

bool HardwareAccel::transferFrameToHost(AVFrame *hwFrame, AVFrame *swFrame)
{
    if (!hwFrame || !swFrame || !initialized_) {
        return false;
    }

    // 如果不是硬件帧，直接返回
    if (hwFrame->format != hwPixFmt_) {
        LOG_WARN("Not a hardware frame");
        return false;
    }

    // 每次使用前先 unref，确保 frame 干净
    av_frame_unref(swFrame);

    // 设置软件帧参数
    swFrame->width = hwFrame->width;
    swFrame->height = hwFrame->height;
    swFrame->format = AV_PIX_FMT_NV12; // 大多数硬件解码器输出NV12格式

    // 分配软件帧缓冲区
    int ret = av_frame_get_buffer(swFrame, 0);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_WARN("Failed to allocate software frame buffer: {}", errBuf);
        return false;
    }

    // 将硬件帧数据传输到软件帧
    ret = av_hwframe_transfer_data(swFrame, hwFrame, 0);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_WARN("Failed to transfer frame data to host: {}", errBuf);
        return false;
    }

    // 复制帧属性
    av_frame_copy_props(swFrame, hwFrame);

    return true;
}

HWAccelType HardwareAccel::getBackendType() const
{
    if ((type_ != HWAccelType::kQsv && type_ != HWAccelType::kAmf) || !hwChildDeviceCtx_) {
        return HWAccelType::kNone;
    }

    AVHWDeviceContext *hwContext = (AVHWDeviceContext *)hwChildDeviceCtx_->data;
    return fromAVHWDeviceType(hwContext->type);
}

std::string HardwareAccel::getDeviceName() const
{
    return getHWAccelTypeName(type_);
}

std::string HardwareAccel::getDeviceDescription() const
{
    return getHWAccelTypeDescription(type_);
}

int HardwareAccel::getDeviceIndex() const
{
    return deviceIndex_;
}

#ifdef VAAPI_AVAILABLE
void *HardwareAccel::getVADisplay() const
{
    AVBufferRef *hwDeviceCtx = nullptr;
    if (type_ == HWAccelType::kVaapi) {
        hwDeviceCtx = hwDeviceCtx_;
    } else if (type_ == HWAccelType::kQsv) {
        hwDeviceCtx = hwChildDeviceCtx_;
    }

    if (!hwDeviceCtx) {
        return nullptr;
    }

    AVHWDeviceContext *deviceContext = (AVHWDeviceContext *)hwDeviceCtx->data;
    if (deviceContext->type != AV_HWDEVICE_TYPE_VAAPI) {
        return nullptr;
    }

    AVVAAPIDeviceContext *vaapiContext =
        reinterpret_cast<AVVAAPIDeviceContext *>(deviceContext->hwctx);
    if (!vaapiContext) {
        return nullptr;
    }

    return vaapiContext->display;
}
#endif

const std::vector<HWAccelInfo> &HardwareAccel::getSupportedHWAccelTypes()
{
    static std::vector<HWAccelInfo> result;
    static std::once_flag initialized;

    std::call_once(initialized, []() {
        LOG_INFO("Start detecting currently supported device types...");

        // 遍历所有硬件设备类型
        AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
        while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
            const auto sdkHwType = fromAVHWDeviceType(type);
            if (sdkHwType == HWAccelType::kNone)
                continue;

            HWAccelInfo info;
            info.type = sdkHwType;
            info.name = av_hwdevice_get_type_name(type);
            info.description = getHWAccelTypeDescription(fromAVHWDeviceType(type));
            if (sdkHwType == HWAccelType::kVulkan) {
                result.push_back(info);
                continue;
            }

            // 检查是否可用
            AVBufferRef *hwDeviceCtx = nullptr;
            int ret = av_hwdevice_ctx_create(&hwDeviceCtx, type, nullptr, nullptr, 0);
            info.available = (ret >= 0);
            if (info.available) {
                // 获取硬件像素格式
                const auto hwPixelFormat = getHWPixelFormatForDevice(type);
                info.hwFormat = utils::avPixelFormat2ImageFormat(hwPixelFormat);

                // 获取支持的软件像素格式
                if (hwPixelFormat != AV_PIX_FMT_NONE) {
                    AVBufferRef *hwFramesCtx = nullptr;
                    AVHWFramesContext *hwFramesCtxData = nullptr;

                    hwFramesCtx = av_hwframe_ctx_alloc(hwDeviceCtx);
                    if (hwFramesCtx) {
                        hwFramesCtxData = (AVHWFramesContext *)hwFramesCtx->data;
                        hwFramesCtxData->format = hwPixelFormat;
                        hwFramesCtxData->sw_format = AV_PIX_FMT_NV12;
                        hwFramesCtxData->width = 1920;
                        hwFramesCtxData->height = 1080;

                        ret = av_hwframe_ctx_init(hwFramesCtx);
                        if (ret >= 0) {
                            AVHWFramesConstraints *constraints =
                                av_hwdevice_get_hwframe_constraints(hwDeviceCtx, nullptr);
                            if (constraints) {
                                for (const AVPixelFormat *p = constraints->valid_sw_formats;
                                     p && *p != AV_PIX_FMT_NONE; p++) {
                                    info.swFormats.push_back(utils::avPixelFormat2ImageFormat(*p));
                                }
                                av_hwframe_constraints_free(&constraints);
                            }
                        }

                        av_buffer_unref(&hwFramesCtx);
                    }
                }

                av_buffer_unref(&hwDeviceCtx);
            }

            result.push_back(info);
        }

        LOG_INFO("End detecting currently supported device types! ");
    });

    return result;
}

std::string HardwareAccel::getHWAccelTypeName(HWAccelType type)
{
    switch (type) {
        case HWAccelType::kNone:
            return "None";
        case HWAccelType::kAuto:
            return "Auto";
        case HWAccelType::kDxva2:
            return "DXVA2";
        case HWAccelType::kD3d11va:
            return "D3D11VA";
        case HWAccelType::kCuda:
            return "CUDA";
        case HWAccelType::kVaapi:
            return "VAAPI";
        case HWAccelType::kVulkan:
            return "Vulkan";
        case HWAccelType::kQsv:
            return "QSV";
        case HWAccelType::kAmf:
            return "AMF";
        default:
            return "Unknown";
    }
}

std::string HardwareAccel::getHWAccelTypeDescription(HWAccelType type)
{
    switch (type) {
        case HWAccelType::kNone:
            return "No hardware acceleration";
        case HWAccelType::kAuto:
            return "Automatically select hardware acceleration";
        case HWAccelType::kDxva2:
            return "DirectX Video Acceleration 2.0";
        case HWAccelType::kD3d11va:
            return "Direct3D 11 Video Acceleration";
        case HWAccelType::kCuda:
            return "NVIDIA CUDA";
        case HWAccelType::kVaapi:
            return "Video Acceleration API (Linux)";
        case HWAccelType::kVulkan:
            return "Vulkan";
        case HWAccelType::kQsv:
            return "Intel Quick Sync Video";
        case HWAccelType::kAmf:
            return "AMD Accelerated Media Framework";
        default:
            return "Unknown hardware acceleration";
    }
}

HWAccelType HardwareAccel::fromAVHWDeviceType(AVHWDeviceType avType)
{
    switch (avType) {
        case AV_HWDEVICE_TYPE_NONE:
            return HWAccelType::kNone;
        case AV_HWDEVICE_TYPE_DXVA2:
            return HWAccelType::kDxva2;
        case AV_HWDEVICE_TYPE_D3D11VA:
            return HWAccelType::kD3d11va;
        case AV_HWDEVICE_TYPE_CUDA:
            return HWAccelType::kCuda;
        case AV_HWDEVICE_TYPE_VAAPI:
            return HWAccelType::kVaapi;
        case AV_HWDEVICE_TYPE_VULKAN:
            return HWAccelType::kVulkan;
        case AV_HWDEVICE_TYPE_QSV:
            return HWAccelType::kQsv;
#ifdef AMF_AVAILABLE
        case AV_HWDEVICE_TYPE_AMF:
            return HWAccelType::kAmf;
#endif
        default:
            return HWAccelType::kNone;
    }
}

AVHWDeviceType HardwareAccel::toAVHWDeviceType(HWAccelType type)
{
    switch (type) {
        case HWAccelType::kNone:
            return AV_HWDEVICE_TYPE_NONE;
        case HWAccelType::kDxva2:
            return AV_HWDEVICE_TYPE_DXVA2;
        case HWAccelType::kD3d11va:
            return AV_HWDEVICE_TYPE_D3D11VA;
        case HWAccelType::kCuda:
            return AV_HWDEVICE_TYPE_CUDA;
        case HWAccelType::kVaapi:
            return AV_HWDEVICE_TYPE_VAAPI;
        case HWAccelType::kVulkan:
            return AV_HWDEVICE_TYPE_VULKAN;
        case HWAccelType::kQsv:
            return AV_HWDEVICE_TYPE_QSV;
#ifdef AMF_AVAILABLE
        case HWAccelType::kAmf:
            return AV_HWDEVICE_TYPE_AMF;
#endif
        case HWAccelType::kAuto:
        default:
            return AV_HWDEVICE_TYPE_NONE;
    }
}

AVPixelFormat HardwareAccel::getHWPixelFormat(AVCodecContext *codecCtx,
                                              const AVPixelFormat *pix_fmts)
{
    // 查找与解码器上下文关联的硬件加速对象
    HardwareAccel *hwAccel = nullptr;
    {
        std::lock_guard<std::mutex> lock(hwAccelMapMutex_);
        auto it = hwAccelMap_.find(codecCtx);
        if (it != hwAccelMap_.end()) {
            hwAccel = it->second;
        }
    }

    if (!hwAccel || !hwAccel->isInitialized()) {
        return AV_PIX_FMT_NONE;
    }

    // 查找硬件像素格式
    AVPixelFormat hwPixFmt = hwAccel->getPixelFormat();
    for (int i = 0; pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
        if (pix_fmts[i] == hwPixFmt) {
#ifdef PLATFORM_IS_WINDOWS
            // 对于D3D11VA，需要设置硬件帧上下文参数
            if (hwPixFmt == AV_PIX_FMT_D3D11) {
                int ret = avcodec_get_hw_frames_parameters(
                    codecCtx, codecCtx->hw_device_ctx, AV_PIX_FMT_D3D11, &codecCtx->hw_frames_ctx);
                if (ret < 0) {
                    LOG_WARN("Failed to allocate HW frames context: {}", utils::avErr2Str(ret));
                    return hwPixFmt;
                }

                auto frames_ctx = (AVHWFramesContext *)codecCtx->hw_frames_ctx->data;
                auto hwctx = (AVD3D11VAFramesContext *)frames_ctx->hwctx;

                // 设置D3D11资源标志，启用共享和着色器资源绑定
                hwctx->MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                hwctx->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;

                ret = av_hwframe_ctx_init(codecCtx->hw_frames_ctx);
                if (ret < 0) {
                    LOG_WARN("Failed to initialize HW frames context: {}", utils::avErr2Str(ret));
                    av_buffer_unref(&codecCtx->hw_frames_ctx);
                    codecCtx->hw_frames_ctx = nullptr;
                    return hwPixFmt;
                }
            }
#endif
            if (hwPixFmt == AV_PIX_FMT_VULKAN) {
                return vulkan_helper::getPixelFormat(codecCtx, pix_fmts);
            }

            return hwPixFmt;
        }
    }

    return AV_PIX_FMT_NONE;
}

bool HardwareAccel::initHWDevice(AVHWDeviceType &deviceType, AVHWDeviceType backendDeviceType,
                                 int deviceIndex, const CreateHWContextCallback &createCallback,
                                 const FreeHWContextCallback &freeCallback)
{
    if (deviceType == AV_HWDEVICE_TYPE_NONE) {
        return false;
    }

    // 创建设备名称
    char deviceName[32] = {0};
    if (deviceIndex > 0) {
        snprintf(deviceName, sizeof(deviceName), "%d", deviceIndex);
    } else {
        snprintf(deviceName, sizeof(deviceName), "auto");
    }

    // 尝试通过用户回调获取硬件设备上下文
    if (createCallback) {
        try {
            // 将AVHWDeviceType转换为HWAccelType用于回调
            HWAccelType sdkType = fromAVHWDeviceType(deviceType);
            void *userHwContext = createCallback(sdkType);

            // 如果是vulkan，且userHwContext为空或不合规，则按照DXVA2或VAAPI进行回退
            if (sdkType == HWAccelType::kVulkan &&
                (!userHwContext || !validateUserHWContext(userHwContext, deviceType))) {
#ifdef OS_WINDOWS
                sdkType = HWAccelType::kDxva2;
                deviceType = AV_HWDEVICE_TYPE_DXVA2;
#elif OS_LINUX
                sdkType = HWAccelType::kVaapi;
                deviceType = AV_HWDEVICE_TYPE_VAAPI;
#else
#endif
                userHwContext = createCallback(sdkType);
                LOG_WARN("Vulkan Accel not supported, change to {}", getHWAccelTypeName(sdkType));
            }

            if (userHwContext) {
                // 验证用户提供的硬件上下文类型是否匹配
                if (validateUserHWContext(userHwContext, deviceType)) {
                    // 从用户提供的硬件上下文创建FFmpeg的hwdevice_ctx
                    int ret = createHWDeviceFromUserContext(userHwContext, deviceType,
                                                            &hwDeviceCtx_, freeCallback);
                    if (ret >= 0) {
                        LOG_INFO(
                            "Successfully created hardware device context from user callback!");
                        isUserContext_ = true;
                        return true;
                    } else {
                        LOG_WARN(
                            "Failed to create FFmpeg hwdevice_ctx from user context, falling back "
                            "to default creation!");
                    }
                } else {
                    LOG_WARN(
                        "User provided hardware context type mismatch for {}, falling back to "
                        "default creation!",
                        av_hwdevice_get_type_name(deviceType));
                }
            } else {
                LOG_DEBUG("User callback returned null context, falling back to default creation!");
            }
        } catch (const std::exception &e) {
            LOG_WARN("Exception caught from user callback: {}, falling back to default creation!",
                     e.what());
        } catch (...) {
            LOG_WARN(
                "Unknown exception caught from user callback, falling back to default creation!");
        }
    }

    // Fallback: 使用FFmpeg的默认创建流程。如果是AMF和QSV需要特殊处理
    int ret = -1;
    if (0
#ifdef QSV_AVAILABLE
        || deviceType == AV_HWDEVICE_TYPE_QSV
#endif
#ifdef AMF_AVAILABLE
        || deviceType == AV_HWDEVICE_TYPE_AMF
#endif
    ) {
        ret = tryDerivedHwContext(deviceType, backendDeviceType, deviceName, createCallback,
                                  freeCallback);
    }

    // 正常的创建流程
    if (ret != 0) {
        ret = av_hwdevice_ctx_create(&hwDeviceCtx_, deviceType, deviceName, nullptr, 0);
    }
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_WARN("Failed to create hardware device context: {}", errBuf);
        return false;
    }

    LOG_INFO("Successfully created hardware device context using FFmpeg default creation");
    return true;
}

AVHWDeviceType HardwareAccel::findBestHWAccelType()
{
    // 优先级顺序：
    // Windows: CUDA > QSV > D3D11VA > DXVA2 > Vulkan (目前暂时只有Windows和FFmpeg
    // >= 5.0时，才支持QSV) Linux: CUDA > VAAPI
#ifdef OS_WINDOWS
    const std::vector<AVHWDeviceType> priorityList = {
        AV_HWDEVICE_TYPE_CUDA,
#ifdef QSV_AVAILABLE
        AV_HWDEVICE_TYPE_QSV,
#endif
#ifdef AMF_AVAILABLE
        AV_HWDEVICE_TYPE_AMF,
#endif
        AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2, AV_HWDEVICE_TYPE_VULKAN};
#elif OS_LINUX
    const std::vector<AVHWDeviceType> priorityList = {
        AV_HWDEVICE_TYPE_CUDA,
        AV_HWDEVICE_TYPE_VAAPI,
    };
#else
    const std::vector<AVHWDeviceType> priorityList;
#endif

    for (const AVHWDeviceType &type : priorityList) {
        if (isAvailableHWAccelType(fromAVHWDeviceType(type))) {
            return type;
        }
    }

    return AV_HWDEVICE_TYPE_NONE;
}

bool HardwareAccel::isAvailableHWAccelType(HWAccelType type) const
{
    const auto &supportedType = getSupportedHWAccelTypes();
    const auto findIter = std::find_if(
        supportedType.begin(), supportedType.end(),
        [type](const HWAccelInfo &info) { return info.available && info.type == type; });

    return findIter != supportedType.end();
}

bool HardwareAccel::validateUserHWContext(void *userContext, AVHWDeviceType expectedType)
{
    if (!userContext) {
        return false;
    }

    switch (expectedType) {
#ifdef D3D11VA_AVAILABLE
        case AV_HWDEVICE_TYPE_D3D11VA: {
            // 检查是否为有效的ID3D11Device指针
            try {
                ID3D11Device *d3dDevice = static_cast<ID3D11Device *>(userContext);
                if (d3dDevice) {
                    ComPtr<IUnknown> test;
                    const HRESULT hr = d3dDevice->QueryInterface(
                        __uuidof(IUnknown), reinterpret_cast<void **>(test.GetAddressOf()));
                    return SUCCEEDED(hr);
                }
            } catch (...) {
                return false;
            }
            break;
        }
#endif
#ifdef DXVA2_AVAILABLE
        case AV_HWDEVICE_TYPE_DXVA2: {
            // 检查是否为有效的IDirect3DDeviceManager9指针
            // 检查是否为有效的ID3D11Device指针
            try {
                IDirect3DDeviceManager9 *mgr = static_cast<IDirect3DDeviceManager9 *>(userContext);
                if (mgr) {
                    ComPtr<IUnknown> test;
                    const HRESULT hr = mgr->QueryInterface(
                        __uuidof(IUnknown), reinterpret_cast<void **>(test.GetAddressOf()));
                    return SUCCEEDED(hr);
                }
            } catch (...) {
                return false;
            }
            break;
        }
#endif
#ifdef CUDA_AVAILABLE
        case AV_HWDEVICE_TYPE_CUDA: {
            // 检查是否为有效的CUcontext，CUDA上下文验证需要CUDA运行时API
            // 暂时不建议上层接管，线程管理很复杂
            CUcontext cuContext = static_cast<CUcontext>(userContext);
            return cuContext != nullptr;
        }
#endif
#ifdef VAAPI_AVAILABLE
        case AV_HWDEVICE_TYPE_VAAPI: {
            // 检查是否为有效的VADisplay
            VADisplay vaDisplay = static_cast<VADisplay>(userContext);
            return vaDisplay != nullptr;
        }
#endif
#ifdef VULKAN_AVAILABLE
        case AV_HWDEVICE_TYPE_VULKAN: {
            return vulkan_helper::vulkanDeviceContextIsValid(userContext);
        }
#endif
        default:
            // 对于其他类型，进行基本的非空检查
            return userContext != nullptr;
    }

    return false;
}

int HardwareAccel::createHWDeviceFromUserContext(void *userContext, AVHWDeviceType deviceType,
                                                 AVBufferRef **hwDeviceCtx,
                                                 const FreeHWContextCallback &freeCallback)
{
    if (!userContext) {
        return AVERROR(EINVAL);
    }

    // 创建硬件设备上下文
    *hwDeviceCtx = av_hwdevice_ctx_alloc(deviceType);
    if (!*hwDeviceCtx) {
        return AVERROR(ENOMEM);
    }

    AVHWDeviceContext *deviceContext = (AVHWDeviceContext *)(*hwDeviceCtx)->data;

    switch (deviceType) {
#ifdef D3D11VA_AVAILABLE
        case AV_HWDEVICE_TYPE_D3D11VA: {
            AVD3D11VADeviceContext *d3d11Context = (AVD3D11VADeviceContext *)deviceContext->hwctx;
            ID3D11Device *d3dDevice = static_cast<ID3D11Device *>(userContext);

            // 增加引用计数，因为FFmpeg会管理这个引用
            d3dDevice->AddRef();
            d3d11Context->device = d3dDevice;

            break;
        }
#endif
#ifdef DXVA2_AVAILABLE
        case AV_HWDEVICE_TYPE_DXVA2: {
            AVDXVA2DeviceContext *dxva2Context = (AVDXVA2DeviceContext *)deviceContext->hwctx;
            IDirect3DDeviceManager9 *deviceManager =
                static_cast<IDirect3DDeviceManager9 *>(userContext);

            // 增加引用计数
            deviceManager->AddRef();
            dxva2Context->devmgr = deviceManager;

            break;
        }
#endif
#ifdef CUDA_AVAILABLE
        case AV_HWDEVICE_TYPE_CUDA: {
            AVCUDADeviceContext *cudaContext = (AVCUDADeviceContext *)deviceContext->hwctx;
            CUcontext cuContext = static_cast<CUcontext>(userContext);

            cudaContext->cuda_ctx = cuContext;
            cudaContext->stream = nullptr;
            // 注意：CUDA上下文的生命周期由用户管理，这里不需要额外的引用计数

            break;
        }
#endif
#ifdef VAAPI_AVAILABLE
        case AV_HWDEVICE_TYPE_VAAPI: {
            AVVAAPIDeviceContext *vaapiContext = (AVVAAPIDeviceContext *)deviceContext->hwctx;
            VADisplay vaDisplay = static_cast<VADisplay>(userContext);

            vaapiContext->display = vaDisplay;
            // VAAPI显示的生命周期由用户管理

            break;
        }
#endif
#ifdef VULKAN_AVAILABLE
        case AV_HWDEVICE_TYPE_VULKAN: {
            vulkan_helper::transToAVVulkanDeviceContext(deviceContext, userContext);
            break;
        }
#endif
        default:
            av_buffer_unref(hwDeviceCtx);
            hwDeviceCtx = nullptr;
            return AVERROR(ENOSYS);
    }

    // 设置释放回调
    if (freeCallback) {
        deviceContext->free = freeHWContextCallback;
        deviceContext->user_opaque =
            new FreeHWContext{fromAVHWDeviceType(deviceType), userContext, freeCallback};
    }

    // 初始化硬件设备上下文
    int ret = av_hwdevice_ctx_init(*hwDeviceCtx);
    if (ret < 0) {
        av_buffer_unref(hwDeviceCtx);
        hwDeviceCtx = nullptr;
        return ret;
    }

    return 0;
}

int HardwareAccel::tryDerivedHwContext(AVHWDeviceType deviceType, AVHWDeviceType backendDeviceType,
                                       const char *deviceName,
                                       const CreateHWContextCallback &createCallback,
                                       const FreeHWContextCallback &freeCallback)
{
    // 先释放已有的派生硬件上下文
    if (hwChildDeviceCtx_) {
        av_buffer_unref(&hwChildDeviceCtx_);
        hwChildDeviceCtx_ = nullptr;
    }

    int ret = -1;

    if (createCallback) {
        // 尝试通过用户回调获取派生硬件设备的上下文
        void *userHwContext = createCallback(fromAVHWDeviceType(backendDeviceType));
        if (userHwContext && validateUserHWContext(userHwContext, backendDeviceType)) {
            // 从用户提供的硬件上下文创建FFmpeg的hwdevice_ctx
            ret = createHWDeviceFromUserContext(userHwContext, backendDeviceType,
                                                &hwChildDeviceCtx_, freeCallback);
        }

        if (ret < 0) {
            LOG_WARN(
                "Failed to create FFmpeg hwdevice_ctx from user context, falling back to use "
                "ffmpeg  creation! Error: {}",
                utils::avErr2Str(ret));
        }
    }

    if (ret < 0) {
        // 使用默认方式创建
        ret = av_hwdevice_ctx_create(&hwChildDeviceCtx_, backendDeviceType, deviceName, nullptr, 0);
        if (ret < 0) {
            LOG_WARN("Failed to create derived context by ffmpeg creation! Error: {}",
                     utils::avErr2Str(ret));
            return ret;
        }
    }

    // 从派生的硬件上下文创建FFmpeg的hwdevice_ctx
    ret = av_hwdevice_ctx_create_derived(&hwDeviceCtx_, deviceType, hwChildDeviceCtx_, 0);

    if (ret < 0) {
        LOG_WARN(
            "Failed to create FFmpeg hwdevice_ctx from derived context, falling back to "
            "default creation! Error: {}",
            utils::avErr2Str(ret));
        return ret;
    }

    // 初始化硬件设备上下文
    ret = av_hwdevice_ctx_init(hwDeviceCtx_);
    if (ret < 0) {
        clearHwCtx();
        LOG_WARN(
            "Failed to init FFmpeg hwdevice_ctx from derived context, falling back to "
            "default creation! Error: {}",
            utils::avErr2Str(ret));
        return ret;
    }

    return ret;
}

void HardwareAccel::clearHwCtx()
{
    if (hwChildDeviceCtx_) {
        av_buffer_unref(&hwChildDeviceCtx_);
        hwChildDeviceCtx_ = nullptr;
    }

    if (hwDeviceCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
    }
}

AVPixelFormat HardwareAccel::getHWPixelFormatForDevice(AVHWDeviceType deviceType)
{
    switch (deviceType) {
        case AV_HWDEVICE_TYPE_DXVA2:
            return AV_PIX_FMT_DXVA2_VLD;
        case AV_HWDEVICE_TYPE_D3D11VA:
            return AV_PIX_FMT_D3D11;
        case AV_HWDEVICE_TYPE_CUDA:
            return AV_PIX_FMT_CUDA;
        case AV_HWDEVICE_TYPE_VAAPI:
            return AV_PIX_FMT_VAAPI;
        case AV_HWDEVICE_TYPE_VDPAU:
            return AV_PIX_FMT_VDPAU;
#ifdef QSV_AVAILABLE
        case AV_HWDEVICE_TYPE_QSV:
            return AV_PIX_FMT_QSV;
#endif
        case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
            return AV_PIX_FMT_VIDEOTOOLBOX;
        case AV_HWDEVICE_TYPE_VULKAN:
            return AV_PIX_FMT_VULKAN;
#ifdef AMF_AVAILABLE
        case AV_HWDEVICE_TYPE_AMF:
            return AV_PIX_FMT_AMF_SURFACE;
#endif
        default:
            return AV_PIX_FMT_NONE;
    }
}

//-----------------------------------------------------------------------------
// HardwareAccelFactory 实现
//-----------------------------------------------------------------------------

HardwareAccelFactory &HardwareAccelFactory::getInstance()
{
    static HardwareAccelFactory instance;
    return instance;
}

std::shared_ptr<HardwareAccel> HardwareAccelFactory::createHardwareAccel(
    HWAccelType type, HWAccelType backendType, int deviceIndex,
    const CreateHWContextCallback &createCallback, const FreeHWContextCallback &freeCallback)
{
    auto hwAccel = std::make_shared<HardwareAccel>();
    if (hwAccel->init(type, backendType, deviceIndex, createCallback, freeCallback)) {
        return hwAccel;
    }
    return nullptr;
}

std::vector<HWAccelInfo> HardwareAccelFactory::getSupportedHWAccelTypes() const
{
    return HardwareAccel::getSupportedHWAccelTypes();
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END