#ifndef DECODER_SDK_BASEDECODER_H
#define DECODER_SDK_BASEDECODER_H
#include <condition_variable>
#include <mutex>
#include <thread>

#include "base/define.h"
#include "utils/PacketQueue.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
}

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class BaseDecoder
{
public:
    BaseDecoder();
    virtual ~BaseDecoder();

protected:
    // 当前正在处理的 AVPacket
    AVPacket *pkt_ = nullptr;
    // 关联的数据包队列（PacketQueue）
    PacketQueue *queue_ = nullptr;
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