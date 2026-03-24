#ifndef DECODER_SDK_INTERNAL_ENCODER_BASE_H
#define DECODER_SDK_INTERNAL_ENCODER_BASE_H

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "base/base_define.h"
#include "base/frame_queue.h"
#include "include/decodersdk/common_define.h"
#include "muxer/muxer.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class EncoderBase {
public:
    /**
     * @brief 构造函数
     * @param muxer 复用器
     */
    explicit EncoderBase(std::shared_ptr<Muxer> muxer);
    virtual ~EncoderBase();

    /**
     * @brief 当前编码器的类型
     * 
     * @param 
     * @return 媒体类型
     */
    virtual MediaType mediaType() const = 0;

    /**
     * @brief 打开编码器
     * @param config 编码配置
     * @return 成功返回true
     */
    virtual bool open(const EncoderConfig &config) = 0;

    /**
     * @brief 启动编码循环
     */
    virtual void start();

    /**
     * @brief 停止编码循环
     */
    virtual void stop();

    /**
     * @brief 关闭编码器
     */
    virtual void close();

    /**
     * @brief 获取可写入的帧，用来应用层往里面填充数据
     * @param frame 可写入的帧
     * @return 可写入的帧指针，失败返回nullptr
     */
    virtual bool getWritableFrame(Frame &frame) const = 0;

    /**
     * @brief 推送帧到编码队列
     * @param frame 待编码帧
     * @return 成功返回true
     */
    bool pushFrame(const Frame &frame);

    /**
     * @brief 刷新编码器（发送空帧以清空缓冲区）
     */
    void flush();

    /**
     * @brief 获取统计信息
     */
    const EncoderStatistics &getStatistics() const;

protected:
    /**
     * @brief 编码循环实现
     */
    virtual void encodeLoop() = 0;

    /**
     * @brief 发送帧给编码器
     * @param frame 帧
     * @return ret code（0 或 AVERROR(EAGAIN)/负数）
     *
     * 说明：
     * - 非阻塞调用；若内部缓冲未就绪可能返回 AVERROR(EAGAIN)
     * - 传入 nullptr 表示 flush（让编码器输出缓存内剩余数据）
     */
    int sendFrameToCodec(const AVFrame *frame);

    /**
     * @brief 从编码器接收包并写入复用器
     * @return ret code（0 表示成功写出一个包；AVERROR(EAGAIN)/AVERROR_EOF/负数表示其他状态）
     *
     * 说明：
     * - 内部会按 muxer 的流时间基进行时间戳缩放（av_packet_rescale_ts）
     * - 成功时将包交给 Muxer 管理，失败时释放 AVPacket
     */
    int receivePacketAndMux();

protected:
    std::shared_ptr<Muxer> muxer_;
    AVCodecContext *codecCtx_ = nullptr;
    std::shared_ptr<FrameQueue> frameQueue_;
    
    std::thread thread_;
    std::atomic<bool> isRunning_{false};
    std::atomic<bool> isOpened_{false};
    
    EncoderStatistics statistics_;
    int streamIndex_ = -1;
    EncoderConfig config_;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_ENCODER_BASE_H
