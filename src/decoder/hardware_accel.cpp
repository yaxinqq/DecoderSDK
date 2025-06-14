#include "hardware_accel.h"

#include <algorithm>

#include "logger/Logger.h"
#include "utils/common_utils.h"

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
      deviceIndex_(0)
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

    // 释放硬件设备上下文
    if (hwDeviceCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
    }

    initialized_ = false;
}

bool HardwareAccel::init(HWAccelType type, int deviceIndex)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 如果已经初始化，先释放资源
    if (hwDeviceCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
    }

    initialized_ = false;
    type_ = type;
    deviceIndex_ = deviceIndex;

    // 如果类型为NONE，直接返回成功
    if (type_ == HWAccelType::kNone) {
        return true;
    }

    // 确定硬件设备类型
    AVHWDeviceType deviceType;
    if (type_ == HWAccelType::kAuto) {
        deviceType = findBestHWAccelType();
        if (deviceType == AV_HWDEVICE_TYPE_NONE) {
            LOG_WARN("No suitable hardware acceleration method found");
            return false;
        }
        type_ = fromAVHWDeviceType(deviceType);
    } else {
        deviceType = toAVHWDeviceType(type_);

        // 检查指定的硬件加速类型是否可用
        AVBufferRef *testDeviceCtx = nullptr;
        int ret = av_hwdevice_ctx_create(&testDeviceCtx, deviceType, nullptr, nullptr, 0);
        if (ret < 0) {
            // Todo: 输出软解退化提示信息
            // 回退到软解
            type_ = HWAccelType::kNone;
            return true;
        }
    }

    // 初始化硬件设备
    if (!initHWDevice(deviceType, deviceIndex_)) {
        LOG_WARN("Failed to initialize hardware device: {}", getHWAccelTypeName(type_));
        return false;
    }

    // 获取硬件像素格式
    hwPixFmt_ = getHWPixelFormatForDevice(deviceType);
    if (hwPixFmt_ == AV_PIX_FMT_NONE) {
        LOG_WARN("Failed to get hardware pixel format for device: {}", getHWAccelTypeName(type_));
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
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

std::string HardwareAccel::getDeviceName() const
{
    return getHWAccelTypeName(type_);
}

std::string HardwareAccel::getDeviceDescription() const
{
    return getHWAccelTypeDescription(type_);
}

std::vector<HWAccelInfo> HardwareAccel::getSupportedHWAccelTypes()
{
    std::vector<HWAccelInfo> result;

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
        case HWAccelType::kVdpau:
            return "VDPAU";
        case HWAccelType::kQsv:
            return "QSV";
        case HWAccelType::kVideoToolBox:
            return "VideoToolbox";
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
        case HWAccelType::kVdpau:
            return "Video Decode and Presentation API for Unix (Linux)";
        case HWAccelType::kQsv:
            return "Intel Quick Sync Video";
        case HWAccelType::kVideoToolBox:
            return "Apple VideoToolbox (macOS/iOS)";
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
        case AV_HWDEVICE_TYPE_VDPAU:
            return HWAccelType::kVdpau;
        case AV_HWDEVICE_TYPE_QSV:
            return HWAccelType::kQsv;
        case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
            return HWAccelType::kVideoToolBox;
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
        case HWAccelType::kVdpau:
            return AV_HWDEVICE_TYPE_VDPAU;
        case HWAccelType::kQsv:
            return AV_HWDEVICE_TYPE_QSV;
        case HWAccelType::kVideoToolBox:
            return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
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
    AVPixelFormat hwPixFmt = hwAccel->getHWPixelFormat();
    for (int i = 0; pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
        if (pix_fmts[i] == hwPixFmt) {
            return hwPixFmt;
        }
    }

    return AV_PIX_FMT_NONE;
}

bool HardwareAccel::initHWDevice(AVHWDeviceType deviceType, int deviceIndex)
{
    if (deviceType == AV_HWDEVICE_TYPE_NONE) {
        return false;
    }

    // 创建设备名称
    char deviceName[32] = {0};
    if (deviceIndex > 0) {
        snprintf(deviceName, sizeof(deviceName), "%d", deviceIndex);
    }

    // 创建硬件设备上下文
    int ret = av_hwdevice_ctx_create(&hwDeviceCtx_, deviceType, deviceName, nullptr, 0);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_WARN("Failed to create hardware device context: {}", errBuf);
        return false;
    }

    return true;
}

AVHWDeviceType HardwareAccel::findBestHWAccelType()
{
    // 优先级顺序：D3D11VA > DXVA2 > QSV > CUDA > VAAPI > VDPAU > VIDEOTOOLBOX
    std::vector<AVHWDeviceType> priorityList = {
        AV_HWDEVICE_TYPE_D3D11VA,     AV_HWDEVICE_TYPE_DXVA2, AV_HWDEVICE_TYPE_CUDA,
        AV_HWDEVICE_TYPE_QSV,         AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_VDPAU,
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX};

    for (AVHWDeviceType type : priorityList) {
        AVBufferRef *hwDeviceCtx = nullptr;
        int ret = av_hwdevice_ctx_create(&hwDeviceCtx, type, nullptr, nullptr, 0);
        if (ret >= 0) {
            av_buffer_unref(&hwDeviceCtx);
            return type;
        }
    }

    return AV_HWDEVICE_TYPE_NONE;
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
        case AV_HWDEVICE_TYPE_QSV:
            return AV_PIX_FMT_QSV;
        case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
            return AV_PIX_FMT_VIDEOTOOLBOX;
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

std::shared_ptr<HardwareAccel> HardwareAccelFactory::createHardwareAccel(HWAccelType type,
                                                                         int deviceIndex)
{
    auto hwAccel = std::make_shared<HardwareAccel>();
    if (hwAccel->init(type, deviceIndex)) {
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