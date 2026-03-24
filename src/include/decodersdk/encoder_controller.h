#ifndef DECODER_SDK_ENCODER_CONTROLLER_H
#define DECODER_SDK_ENCODER_CONTROLLER_H

#include "common_define.h"
#include "frame.h"
#include <memory>
#include <string>

namespace decoder_sdk {

namespace internal {
class EncoderController;
}

class DECODER_SDK_API EncoderController {
public:
    /**
     * @brief 构造函数
     */
    EncoderController();

    /**
     * @brief 析构函数
     */
    ~EncoderController();

    /**
     * @brief 打开编码器
     * @param url 输出URL
     * @param config 编码配置
     * @return 成功返回true
     */
    bool open(const std::string &url, const EncoderConfig &config);

    /**
     * @brief 关闭编码器
     */
    void close();

    /**
     * @brief 启动编码
     */
    void start();

    /**
     * @brief 停止编码
     */
    void stop();

    /**
     * @brief 获取可写帧
     * @param mediaType 媒体类型
     * @return 可写帧，使用前需检查isValid
     */
    Frame getWriteableFrame(MediaType mediaType) const;

    /**
     * @brief 推送帧
     * @param mediaType 媒体类型
     * @param frame 帧
     * @return 成功返回true
     */
    bool pushFrame(MediaType mediaType, const Frame &frame);

private:
    std::unique_ptr<internal::EncoderController> impl_;
};

} // namespace decoder_sdk

#endif // DECODER_SDK_ENCODER_CONTROLLER_H
