#pragma once
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <memory>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <mutex>

/**
 * @brief 硬件加速类型枚举
 */
enum class HWAccelType
{
    NONE,        // 不使用硬件加速
    AUTO,        // 自动选择最佳硬件加速
    DXVA2,       // DirectX Video Acceleration 2.0
    D3D11VA,     // Direct3D 11 Video Acceleration
    CUDA,        // NVIDIA CUDA
    VAAPI,       // Video Acceleration API (Linux)
    VDPAU,       // Video Decode and Presentation API for Unix (Linux)
    QSV,         // Intel Quick Sync Video
    VIDEOTOOLBOX // Apple VideoToolbox (macOS/iOS)
};

/**
 * @brief 硬件加速信息结构体
 */
struct HWAccelInfo
{
    AVHWDeviceType type;                  // 硬件设备类型
    std::string name;                     // 名称
    std::string description;              // 描述
    bool available;                       // 是否可用
    AVPixelFormat hwFormat;               // 硬件像素格式
    std::vector<AVPixelFormat> swFormats; // 支持的软件像素格式
};

/**
 * @brief 硬件加速上下文类
 */
class HardwareAccel
{
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
     * @return 是否初始化成功
     */
    bool init(HWAccelType type = HWAccelType::AUTO, int deviceIndex = 0);

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
    HWAccelType getType() const { return type_; }

    /**
     * @brief 获取硬件像素格式
     * @return 硬件像素格式
     */
    AVPixelFormat getHWPixelFormat() const { return hwPixFmt_; }

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
     * @brief 是否已初始化
     * @return 是否已初始化
     */
    bool isInitialized() const { return initialized_; }

    /**
     * @brief 是否需要将硬件帧转换到主机内存
     * @return 是否需要转换
     */
    bool isTransferToHostRequired() const { return transferToHostRequired_; }

    /**
     * @brief 设置是否需要将硬件帧转换到主机内存
     * @param required 是否需要转换
     */
    void setTransferToHostRequired(bool required) { transferToHostRequired_ = required; }

    /**
     * @brief 获取支持的硬件加速类型列表
     * @return 硬件加速类型列表
     */
    static std::vector<HWAccelInfo> getSupportedHWAccelTypes();

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
     * @return 是否初始化成功
     */
    bool initHWDevice(AVHWDeviceType deviceType, int deviceIndex);

    /**
     * @brief 查找最佳硬件加速类型
     * @return 最佳硬件加速类型
     */
    AVHWDeviceType findBestHWAccelType();

    /**
     * @brief 获取硬件像素格式
     * @param deviceType 设备类型
     * @return 硬件像素格式
     */
    static AVPixelFormat getHWPixelFormatForDevice(AVHWDeviceType deviceType);

private:
    HWAccelType type_;                    // 硬件加速类型
    AVBufferRef *hwDeviceCtx_;            // 硬件设备上下文
    AVPixelFormat hwPixFmt_;              // 硬件像素格式
    bool initialized_;                    // 是否已初始化
    bool transferToHostRequired_ = false; // 是否需要将硬件帧转换到主机内存
    int deviceIndex_;                     // 设备索引
    std::mutex mutex_;                    // 互斥锁

    // 静态成员，用于存储硬件加速上下文
    static std::map<AVCodecContext *, HardwareAccel *> hwAccelMap_;
    static std::mutex hwAccelMapMutex_;
};

/**
 * @brief 硬件加速工厂类
 */
class HardwareAccelFactory
{
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
     * @return 硬件加速上下文指针
     */
    std::shared_ptr<HardwareAccel> createHardwareAccel(HWAccelType type = HWAccelType::AUTO, int deviceIndex = 0);

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