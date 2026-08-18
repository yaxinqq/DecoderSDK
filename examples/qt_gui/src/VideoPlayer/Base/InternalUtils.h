#pragma once
#include "decodersdk/common_define.h"

#include <QString>

#define EPSILON 1e-6
#define DOUBLEEPSILON 1e-12

// 内部工具函数，不能在dll之外使用
namespace utils {
    /**
     * @brief 字符串转为decoder_sdk::ImageFormat
     *
     * @param str 字符串
     * @return 对应的decoder_sdk::ImageFormat
     */
    decoder_sdk::ImageFormat transImageFormatString(const QString &str);
    /**
     * @brief 字符串转为decoder_sdk::HWAccelType
     *
     * @param str 字符串
     * @return 对应的decoder_sdk::HWAccelType
     */
    decoder_sdk::HWAccelType transHwAccelTypeString(const QString &str);
    /**
     * @brief 字符串转为decoder_sdk::RequiredMediaType
     *
     * @param str 字符串
     * @return 对应的decoder_sdk::RequiredMediaType
     */
    decoder_sdk::MediaTypes transRequiredMediaTypeString(const QString &str);
    /**
     * @brief 字符串转为decoder_sdk::RtspTransport
     *
     * @param str 字符串
     * @return 对应的decoder_sdk::RtspTransport
     */
    decoder_sdk::DecoderConfig::RtspTransport transRtspTransportString(const QString &str);

    /*
     * @brief 创建硬件上下文的回调
     *
     * @param type 硬件类型
     */
    void *createHwContextCallback(decoder_sdk::HWAccelType type);
    /*
     * @brief 销毁硬件上下文的回调
     *
     * @param type 硬件类型
     * @param userHwContext 用户创建的硬件上下文
     */
    void freeHwContextCallback(decoder_sdk::HWAccelType type, void *userHwContext);

    /**
     * @brief 从录制事件中得到录像文件路径
     *
     * @param event 录制事件
     * @return 录像文件路径
     */
    QString getFilePathFromRecordEvent(const std::shared_ptr<decoder_sdk::EventArgs> &event);

    // 浮点数的大小比较函数
    bool equal(double a, double b, double epsilon = DOUBLEEPSILON);
    bool equal(float a, float b, double epsilon = EPSILON);

    // a > b 返回true
    bool greater(double a, double b, double epsilon = DOUBLEEPSILON);
    bool greater(float a, float b, double epsilon = EPSILON);

    // a >= b 返回true
    bool greaterAndEqual(double a, double b, double epsilon = DOUBLEEPSILON);
    bool greaterAndEqual(float a, float b, double epsilon = EPSILON);
} // namespace utils