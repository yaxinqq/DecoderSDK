#include "AudioDecoder.h"

AudioDecoder::AudioDecoder(std::shared_ptr<Demuxer> demuxer)
    : DecoderBase(demuxer)
{
    // 初始化音频时钟
    clock_.init(-1);
}

AudioDecoder::~AudioDecoder()
{
    close();
}

void AudioDecoder::decodeLoop()
{
    AVFrame* frame = av_frame_alloc();
    if (!frame)
        return;
    
    auto packetQueue = demuxer_->packetQueue(type());
    if (!packetQueue)
    {
        av_frame_free(&frame);
        return;
    }
    
    int serial = packetQueue->serial();
    clock_.init(serial);
    
    while (isRunning_)
    {
        // 检查序列号是否变化
        if (serial != packetQueue->serial())
        {
            avcodec_flush_buffers(codecCtx_);
            serial = packetQueue->serial();
            frameQueue_.setSerial(serial);
            clock_.init(serial);
        }
        
        // 获取一个可写入的帧
        Frame* outFrame = frameQueue_.peekWritable();
        if (!outFrame)
            break;
        
        // 从包队列中获取一个包
        Packet packet;
        bool gotPacket = packetQueue->pop(packet, 100);
        
        if (!gotPacket)
        {
            // 没有包可用，可能是队列为空或已中止
            if (packetQueue->isAbort())
                break;
            continue;
        }
        
        // 检查序列号
        if (packet.serial() != serial)
            continue;
        
        // 发送包到解码器
        int ret = avcodec_send_packet(codecCtx_, packet.get());
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            continue;
        
        // 接收解码后的帧
        ret = avcodec_receive_frame(codecCtx_, frame);
        if (ret < 0)
        {
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                continue;
            break;
        }
        
        // 设置帧属性
        AVRational tb = stream_->time_base;
        
        // 计算帧持续时间
        double duration = frame->nb_samples / (double)codecCtx_->sample_rate;
        
        // 计算PTS
        double pts = frame->pts != AV_NOPTS_VALUE ? frame->pts * av_q2d(tb) : NAN;
        
        // 更新音频时钟
        if (!std::isnan(pts)) {
            clock_.setClock(pts + duration, serial);
        }
        
        // 将解码后的帧复制到输出帧
        *outFrame = Frame(frame);
        outFrame->setSerial(serial);
        outFrame->setDuration(duration);
        // outFrame->setPts(pts);
        
        // 推入帧队列
        frameQueue_.push();
    }
    
    av_frame_free(&frame);
}

AVMediaType AudioDecoder::type() const
{
    return AVMEDIA_TYPE_AUDIO;
}