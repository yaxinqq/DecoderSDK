#ifndef DECODER_SDK_DECODER_CONTROLLER
#define DECODER_SDK_DECODER_CONTROLLER
#include "common_define.h"
#include "frame_queue.h"

namespace decoder_sdk {

namespace internal {
class DecoderController;
} // namespace internal

class DECODER_SDK_API DecoderController {
public:
    /**
     * @brief 构造函数
     */
    DecoderController();
    /**
     * @brief 析构函数
     */
    ~DecoderController();

    /**
     * @brief 同步打开媒体文件
     *
     * @param filePath 媒体文件路径
     * @param config 配置参数
     * @return true 成功
     * @return false 失败
     */
    bool open(const std::string &filePath, const Config &config = Config());

    /**
     * @brief 异步打开媒体文件
     *
     * @param filePath 媒体文件路径
     * @param config 配置参数
     * @param callback 回调函数
     */
    void openAsync(const std::string &filePath, const Config &config, AsyncOpenCallback callback);

    /**
     * @brief 关闭解码器
     *
     * @return true 成功,false 失败
     */
    bool close();

    /**
     * @brief 暂停解码
     *
     * @return true 成功; false 失败
     */
    bool pause();
    /**
     * @brief 恢复解码
     *
     * @return true 成功; false 失败
     */
    bool resume();

    /**
     * @brief 开始解码
     *
     * @return true 成功; false 失败
     */
    bool startDecode();
    /**
     * @brief 停止解码
     *
     * @return true 成功; false 失败
     */
    bool stopDecode();

    /**
     * @brief 定位
     *
     * @param position 定位时间位置
     * @return true 成功; false 失败
     */
    bool seek(double position);
    /**
     * @brief 设置播放速度
     *
     * @param speed 播放速度
     * @return true 成功; false 失败
     */
    bool setSpeed(double speed);

    /**
     * @brief 获取视频帧队列
     *
     * @return 视频帧队列
     */
    FrameQueue videoQueue();

    /**
     * @brief 获取音频帧队列
     *
     * @return 音频帧队列
     */
    FrameQueue audioQueue();

    /**
     * @brief 设置主时钟类型
     *
     * @param type 主时钟类型
     */
    void setMasterClock(MasterClock type);

    /**
     * @brief 获取视频帧率
     *
     * @return 视频帧率
     */
    double getVideoFrameRate() const;

    /**
     * @brief 设置是否启用帧率控制
     *
     * @param enable true 启用; false 禁用
     */
    void setFrameRateControl(bool enable);

    /**
     * @brief 获取是否启用帧率控制
     *
     * @return true 启用; false 禁用
     */
    bool isFrameRateControlEnabled() const;

    /**
     * @brief 获取当前播放速度
     *
     * @return 播放速度
     */
    double curSpeed() const;

    /**
     * @brief 开始录像，暂时将输出文件保存为.mp4格式
     *
     * @param outputPath 输出路径
     * @return true 成功; false 失败
     */
    bool startRecording(const std::string &outputPath);
    /**
     * @brief 停止录像
     *
     * @return true 成功; false 失败
     */
    bool stopRecording();
    /**
     * @brief 获取是否正在录像
     *
     * @return true 正在录像; false 未录像
     */
    bool isRecording() const;

    /**
     * @brief 取消异步打开操作
     */
    void cancelAsyncOpen();

    /**
     * @brief 检查是否有异步打开操作正在进行
     *
     * @return true 正在进行; false 未进行
     */
    bool isAsyncOpenInProgress() const;

    /**
     * @brief 设置全部事件的监听器
     *
     * @param callback 回调函数
     * @return 全局事件监听器句柄
     */
    GlobalEventListenerHandle addGlobalEventListener(EventCallback callback);
    /**
     * @brief 移除全局事件监听器
     *
     * @param handle 全局事件监听器句柄
     * @return true 成功; false 失败
     */
    bool removeGlobalEventListener(const GlobalEventListenerHandle &handle);
    /**
     * @brief 添加事件监听器
     *
     * @param eventType 事件类型
     * @param callback 回调函数
     * @return 事件监听器句柄
     */
    EventListenerHandle addEventListener(EventType eventType, EventCallback callback);
    /**
     * @brief 移除事件监听器
     *
     * @param eventType 事件类型
     * @param handle 事件监听器句柄
     * @return true 成功; false 失败
     */
    bool removeEventListener(EventType eventType, EventListenerHandle handle);

    /**
     * @brief 检查是否正在重连
     *
     * @return true 正在重连; false 未重连
     */
    bool isReconnecting() const;

    /**
     * @brief 获取预缓冲状态
     *
     * @return PreBufferState 预缓冲状态
     */
    PreBufferState getPreBufferState() const;
    /**
     * @brief 获取预缓冲进度
     *
     * @return PreBufferProgress 预缓冲进度
     */
    PreBufferProgress getPreBufferProgress() const;

private:
    std::unique_ptr<internal::DecoderController> impl_;
};

} // namespace decoder_sdk

#endif // DECODER_SDK_INTERNAL_DECODER_CONTROLLER