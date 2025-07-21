#ifndef DECODER_SDK_ZOHE_WS_DECODER_CONTROLLER
#define DECODER_SDK_ZOHE_WS_DECODER_CONTROLLER
#include "common_define.h"
#include "frame.h"

namespace decoder_sdk {

namespace internal {
class ZoheWsDecoderController;
} // namespace internal

class DECODER_SDK_API ZoheWsDecoderController {
public:
    /**
     * @brief 构造函数
     */
    explicit ZoheWsDecoderController(const Config &config);
    /**
     * @brief 析构函数
     */
    ~ZoheWsDecoderController();

    /**
     * @brief 初始化解码器
     * @param enc 编码格式，用来确定解码器ID
     * @param width 视频宽度
     * @param height 视频高度
     * @param extraData 额外数据 (SPS/PPS等)
     * @param extraDataSize 额外数据大小
     * @return true 成功, false 失败
     */
    bool initDecoder(const std::string &enc, int width, int height,
                     const uint8_t *extraData = nullptr, int extraDataSize = 0);

    /**
     * @brief 设置帧回调函数
     * @param callback 回调函数
     * @param userData 用户数据
     */
    void setFrameCallback(std::function<void(const decoder_sdk::Frame &frame)> callback);

    /**
     * @brief 推送数据包进行解码
     * @param data 数据包数据
     * @param size 数据包大小
     * @return true 成功, false 失败
     */
    bool pushPacket(const uint8_t *data, int size);

    /**
     * @brief 刷新解码器 (获取缓冲的帧)
     */
    void flush();

    /**
     * @brief 清理解码器
     */
    void cleanup();

    /**
     * @brief 检查解码器是否已初始化
     * @return true 已初始化, false 未初始化
     */
    bool isInitialized() const;

private:
    std::unique_ptr<internal::ZoheWsDecoderController> impl_;
};

} // namespace decoder_sdk

#endif // DECODER_SDK_ZOHE_WS_DECODER_CONTROLLER