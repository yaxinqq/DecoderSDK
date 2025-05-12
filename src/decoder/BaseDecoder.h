#ifndef DECODER_SDK_BASEDECODER_H
#define DECODER_SDK_BASEDECODER_H
#include <condition_variable>
#include <mutex>
#include <thread>

#include "base/define.h"
#include "utils/FrameQueue.h"
#include "utils/PacketQueue.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
#include <libavformat/avformat.h>
}

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class BaseDecoder
{
public:
    BaseDecoder();
    virtual ~BaseDecoder();

    /*
     * @brief 解码器初始化
     * @param formatCtx 格式上下文（FFmpeg的AVFormatContext）
     * @param avctx 解码器上下文（FFmpeg 的 AVCodecContext）
     * @param packetQueue 关联的数据包队列（PacketQueue）
     * @param frameQueue 关联的帧队列（FrameQueue）
     * @param empty_queue_cond 队列空时用于唤醒等待的条件变量（比如播放结束检测）
     * @return 0 成功，其他失败
     */
    virtual int init(AVFormatContext *formatCtx, AVCodecContext *avctx, PacketQueue *packetQueue, FrameQueue *frameQueue, std::condition_variable *empty_queue_cond);

    /*
     * @brief 启动解码器线程
     *
     * @return 0 成功，其他失败
     */
    virtual int start() = 0;

    /*
     * @brief 解码视频帧
     *
     * @param frame 视频帧/音频帧
     * @param sub 字幕，可能为空
     * @return 0 成功，其他失败
     */
    virtual int decodeFrame(AVFrame *frame, AVSubtitle *sub = nullptr) = 0;

    /*
     * @brief 中止解码器
     */
    virtual void abort();

    /*
     * @brief 销毁解码器
     * @return 0 成功，其他失败
     */
    virtual void destroy();

protected:
    struct AVPacketDeleter {
        void operator()(AVPacket *pkt) {
            if (pkt) {
                av_packet_free(&pkt);
            }
        }
    };

    // 当前正在处理的 AVPacket
    std::unique_ptr<AVPacket, AVPacketDeleter> pkt_;
    // 关联的数据包队列（PacketQueue）
    PacketQueue *pktQueue_ = nullptr;
    // 关联的帧队列（FrameQueue）
    FrameQueue *frameQueue_ = nullptr;
    // 格式上下文外部传入，生命周期由外部负责
    AVFormatContext *formatCtx_ = nullptr;
    // 解码器上下文（FFmpeg 的 AVCodecContext）
    AVCodecContext *codecCtx_ = nullptr;

    // 当前解码包的序列号（用于流切换时识别）
    int pktSerial_ = 0;
    // 是否解码结束
    bool finished_ = false;
    // 是否有一个待处理的 packet（有的话先不取新包）
    bool packetPending_ = false;

    // 解码起始时间戳（如果有的话，单位是 start_pts_tb）
    int64_t startPts_ = 0;
    // start_pts 的时间基（time base）
    AVRational startPtsTb;

    // 预期下一帧的时间戳（PTS）
    int64_t nextPts_ = 0;
    // next_pts 的时间基
    AVRational nextPtsTb;

    // 关联的解码线程
    std::thread thread_;

    // 外部传入, 队列空时用于唤醒等待的条件变量（比如播放结束检测）
    std::condition_variable *emptyQueueCond_ = nullptr;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_BASEDECODER_H