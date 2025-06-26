#ifndef DECODER_SDK_FRAME_H
#define DECODER_SDK_FRAME_H
#include <memory>

#include "common_define.h"
#include "sdk_global.h"

namespace decoder_sdk {
namespace internal {
class Frame;
}

class DECODER_SDK_API Frame {
public:
    /**
     * @brief 构造函数
     */
    Frame();
    /**
     * @brief 构造函数
     * @param frame 内部帧
     */
    explicit Frame(std::unique_ptr<internal::Frame> frame);
    /**
     * @brief 拷贝构造函数
     * @param other 要拷贝的Frame对象
     */
    Frame(const Frame &other);
    /**
     * @brief 析构函数
     */
    ~Frame();

    /**
     * @brief 拷贝赋值函数
     * @param other 要拷贝的Frame对象
     * @return Frame& 拷贝后的Frame对象
     */
    Frame &operator=(const Frame &other);
    /**
     * @brief 移动构造函数
     * @param other 要移动的Frame对象
     */
    Frame(Frame &&other) noexcept;
    /**
     * @brief 移动赋值函数
     * @param other 要移动的Frame对象
     * @return Frame& 移动后的Frame对象
     */
    Frame &operator=(Frame &&other) noexcept;

    /**
     * @brief 判断帧是否有效
     * @return true 有效, false 无效
     */
    bool isValid() const;

    /**
     * @brief 获得帧时长
     * @return double 帧时长
     */
    double durationByFps() const;
    /**
     * @brief 是否是硬解码
     * @return true 是, false 否
     */
    bool isInHardware() const;
    /**
     * @brief 获得解码时间戳（单位s）
     * @return double 解码时间戳
     */
    double secPts() const;

    /**
     * @brief 获得图像宽度
     * @return int 图像宽度
     */
    int width() const;
    /**
     * @brief 获得图像高度
     * @return int 图像高度
     */
    int height() const;
    /**
     * @brief 获得像素格式
     * @return ImageFormat 像素格式
     */
    ImageFormat pixelFormat() const;
    /**
     * @brief 获得AV展示时间戳
     * @return int64_t AV展示时间戳
     */
    int64_t avPts() const;
    /**
     * @brief 获得AV解码时间戳
     * @return int64_t AV解码时间戳
     */
    int64_t pktDts() const;
    /**
     * @brief 获得关键帧标识
     * @return int 关键帧标识
     */
    int keyFrame() const;
    /**
     * @brief 获得推测时间戳
     * @return int64_t 推测时间戳
     */
    int64_t bestEffortTimestamp() const;

    /**
     * @brief 获得采样率
     * @return int 采样率
     */
    int sampleRate() const;
    /**
     * @brief 获得样本数量
     * @return int 样本数量
     */
    int64_t nbSamples() const;

    /**
     * @brief 获得数据指针
     * @param plane 平面索引
     * @return uint8_t* 数据指针
     */
    uint8_t *data(int plane = 0) const;
    /**
     * @brief 获得行大小
     * @param plane 平面索引
     * @return int 行大小
     */
    int linesize(int plane = 0) const;

    /**
     * @brief 获得是否音频帧
     * @return true 音频帧, false 视频帧
     */
    bool isAudioFrame() const;
    /**
     * @brief 获得是否视频帧
     * @return true 视频帧, false 音频帧
     */
    bool isVideoFrame() const;

    /**
     * @brief 获取帧的字节大小
     * @return int 帧的字节大小
     */
    int getBufferSize() const;

private:
    std::unique_ptr<internal::Frame> impl_;
};
} // namespace decoder_sdk

#endif // DECODER_SDK_FRAME_H