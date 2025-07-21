#ifndef DECODER_SDK_INTERNAL_HARDWARE_ACCEL_H
#define DECODER_SDK_INTERNAL_HARDWARE_ACCEL_H
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include "base/base_define.h"
#include "include/decodersdk/common_define.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

/**
 * @brief 硬件加速上下文类
 */
class HardwareAccel {
public:
    /**
     * @brief 构造函数
     */
    HardwareAccel();

    /**
     * @brief 析构函数
     */
    ~HardwareAccel();

    /**
     * @brief 初始化硬件加速
     * @param type 硬件加速类型
     * @param deviceIndex 设备索引
     * @param callback 创建硬件上下文的回调
     * @return 是否初始化成功
     */
    bool init(HWAccelType type = HWAccelType::kAuto, int deviceIndex = 0,
              const CreateHWContextCallback &callback = nullptr);

    /**
     * @brief 设置解码器上下文
     * @param codecCtx 解码器上下文
     * @return 是否设置成功
     */
    bool setupDecoder(AVCodecContext *codecCtx);

    /**
     * @brief 获取硬件帧
     * @param frame 帧
     * @return 硬件帧
     */
    AVFrame *getHWFrame(AVFrame *frame);

    /**
     * @brief 将硬件帧转换为软件帧
     * @param hwFrame 硬件帧
     * @param swFrame 软件帧
     * @return 是否转换成功
     */
    bool transferFrameToHost(AVFrame *hwFrame, AVFrame *swFrame);

    /**
     * @brief 获取硬件加速类型
     * @return 硬件加速类型
     */
    HWAccelType getType() const
    {
        return type_;
    }

    /**
     * @brief 获取硬件像素格式
     * @return 硬件像素格式
     */
    AVPixelFormat getHWPixelFormat() const
    {
        return hwPixFmt_;
    }

    /**
     * @brief 获取设备名称
     * @return 设备名称
     */
    std::string getDeviceName() const;

    /**
     * @brief 获取设备描述
     * @return 设备描述
     */
    std::string getDeviceDescription() const;

    /**
     * @brief 获取设备序号
     * @return 设备序号
     */
    int getDeviceIndex() const;

    /**
     * @brief 是否已初始化
     * @return 是否已初始化
     */
    bool isInitialized() const
    {
        return initialized_;
    }

    /**
     * @brief 获取支持的硬件加速类型列表
     * @return 硬件加速类型列表
     */
    static const std::vector<HWAccelInfo> &getSupportedHWAccelTypes();

    /**
     * @brief 获取硬件加速类型名称
     * @param type 硬件加速类型
     * @return 硬件加速类型名称
     */
    static std::string getHWAccelTypeName(HWAccelType type);

    /**
     * @brief 获取硬件加速类型描述
     * @param type 硬件加速类型
     * @return 硬件加速类型描述
     */
    static std::string getHWAccelTypeDescription(HWAccelType type);

    /**
     * @brief 将AVHWDeviceType转换为HWAccelType
     * @param avType AVHWDeviceType
     * @return HWAccelType
     */
    static HWAccelType fromAVHWDeviceType(AVHWDeviceType avType);

    /**
     * @brief 将HWAccelType转换为AVHWDeviceType
     * @param type HWAccelType
     * @return AVHWDeviceType
     */
    static AVHWDeviceType toAVHWDeviceType(HWAccelType type);

    /**
     * @brief 获取硬件帧的像素格式回调函数
     * @param codecCtx 解码器上下文
     * @param pix_fmts 像素格式列表
     * @return 选择的像素格式
     */
    static AVPixelFormat getHWPixelFormat(AVCodecContext *codecCtx, const AVPixelFormat *pix_fmts);

private:
    /**
     * @brief 初始化硬件设备
     * @param deviceType 设备类型
     * @param deviceIndex 设备索引
     * @param callback 创建硬件上下文的回调
     * @return 是否初始化成功
     */
    bool initHWDevice(AVHWDeviceType deviceType, int deviceIndex,
                      const CreateHWContextCallback &callback = nullptr);

    /**
     * @brief 查找最佳硬件加速类型
     * @return 最佳硬件加速类型
     */
    AVHWDeviceType findBestHWAccelType();

    /**
     * @brief 判断硬件加速类型是否可用
     * @param type 硬件加速类型
     * @return 是否可用
     */
    bool isAvailableHWAccelType(HWAccelType type) const;

    /**
     * @brief 验证用户提供的硬件上下文类型
     * @param userContext 用户提供的硬件上下文
     * @param expectedType 期望的硬件设备类型
     * @return 是否类型匹配且有效
     */
    bool validateUserHWContext(void *userContext, AVHWDeviceType expectedType);

    /**
     * @brief 从用户上下文创建FFmpeg的hwdevice_ctx
     * @param userContext 用户提供的硬件上下文
     * @param deviceType 硬件设备类型
     * @return 创建结果，0表示成功，负值表示错误
     */
    int createHWDeviceFromUserContext(void *userContext, AVHWDeviceType deviceType);

    /**
     * @brief 获取硬件像素格式
     * @param deviceType 设备类型
     * @return 硬件像素格式
     */
    static AVPixelFormat getHWPixelFormatForDevice(AVHWDeviceType deviceType);

private:
    HWAccelType type_;         // 硬件加速类型
    AVBufferRef *hwDeviceCtx_; // 硬件设备上下文
    AVPixelFormat hwPixFmt_;   // 硬件像素格式
    bool initialized_;         // 是否已初始化
    int deviceIndex_;          // 设备索引
    std::mutex mutex_;         // 互斥锁

    // 静态成员，用于存储硬件加速上下文
    static std::map<AVCodecContext *, HardwareAccel *> hwAccelMap_;
    static std::mutex hwAccelMapMutex_;
};

/**
 * @brief 硬件加速工厂类
 */
class HardwareAccelFactory {
public:
    /**
     * @brief 获取单例实例
     * @return 单例实例引用
     */
    static HardwareAccelFactory &getInstance();

    /**
     * @brief 创建硬件加速上下文
     * @param type 硬件加速类型
     * @param deviceIndex 设备索引
     * @param callback 创建硬件上下文的回调
     * @return 硬件加速上下文指针
     */
    std::shared_ptr<HardwareAccel> createHardwareAccel(
        HWAccelType type = HWAccelType::kAuto, int deviceIndex = 0,
        const CreateHWContextCallback &callback = nullptr);

    /**
     * @brief 获取支持的硬件加速类型列表
     * @return 硬件加速类型列表
     */
    std::vector<HWAccelInfo> getSupportedHWAccelTypes() const;

private:
    /**
     * @brief 构造函数
     */
    HardwareAccelFactory() = default;

    /**
     * @brief 析构函数
     */
    ~HardwareAccelFactory() = default;

    // 禁止拷贝和赋值
    HardwareAccelFactory(const HardwareAccelFactory &) = delete;
    HardwareAccelFactory &operator=(const HardwareAccelFactory &) = delete;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_HARDWARE_ACCEL_H
