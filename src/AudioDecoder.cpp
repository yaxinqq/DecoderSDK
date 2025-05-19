#include "AudioDecoder.h"
#include "Utils.h"

extern "C" {
#include "libavutil/opt.h"
#include <libavutil/time.h>
}

AudioDecoder::AudioDecoder(std::shared_ptr<Demuxer> demuxer, std::shared_ptr<SyncController> syncController)
    : DecoderBase(demuxer, syncController)
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

        // 当播放速度改变时，重新初始化重采样上下文
        static float lastSpeed = 1.0f;
        if (std::abs(lastSpeed - speed_.load()) > 0.01f) {
            initResampleContext();
            lastSpeed = speed_.load();
        }

        // 如果需要，重采样音频数据
        if (needResample_ && swrCtx_) {
            AVFrame* resampledFrame = resampleFrame(frame);
            if (resampledFrame != frame) {
                // 如果重采样成功，使用重采样后的帧
                av_frame_free(&frame);
                frame = resampledFrame;
            }
        }
        
        // 设置帧属性
        AVRational tb = stream_->time_base;
        
        // 计算帧持续时间
        double duration = frame->nb_samples / (double)codecCtx_->sample_rate;
        
        // 计算PTS
        double pts = frame->pts != AV_NOPTS_VALUE ? frame->pts * av_q2d(tb) : NAN;
        
        // 更新音频时钟
        // if (!std::isnan(pts)) {
        //     clock_.setClockSpeed(speed_.load());
        //     clock_.setClock(pts + duration, serial);
        // }
        
        // 将解码后的帧复制到输出帧
        *outFrame = Frame(frame);
        outFrame->setSerial(serial);
        outFrame->setDuration(duration);
        outFrame->setPts(pts);

        // 计算延时
        // 如果启用了帧率控制，则根据帧率控制推送速度
        if (frameRateControlEnabled_) {
            double displayTime = calculateFrameDisplayTime(pts, duration * 1000.0);
            if (!utils::greater(displayTime, 0.0)) {
            //    continue;
            } else {
                utils::highPrecisionSleep(displayTime);
            }
        }
        
        // 推入帧队列
        frameQueue_.push();
    }
    
    av_frame_free(&frame);
}

int AudioDecoder::calculateMaxPacketCount() const
{
    // 基础队列长度：约0.5秒的音频数据
    const int baseAudioCount = 25; 
    
    // 根据播放速度调整，但设置上限
    int maxCount = std::min(
        static_cast<int>(baseAudioCount * speed_),
        baseAudioCount * 3
    );
    
    // 确保队列长度在合理范围内
    return std::clamp(maxCount, baseAudioCount, 100);
}

bool AudioDecoder::initResampleContext()
{
    // 释放旧的重采样上下文
    if (swrCtx_) {
        swr_free(&swrCtx_);
        swrCtx_ = nullptr;
    }
    
    // 如果播放速度接近1.0，不需要重采样
    if (std::abs(speed_.load() - 1.0f) < 0.01f) {
        needResample_ = false;
        return true;
    }
    
    needResample_ = true;
    
    // 创建重采样上下文
    swrCtx_ = swr_alloc();
    if (!swrCtx_) {
        return false;
    }
    
    // 设置输入参数
    av_opt_set_chlayout(swrCtx_, "in_chlayout", &codecCtx_->ch_layout, 0);
    av_opt_set_int(swrCtx_, "in_sample_rate", codecCtx_->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx_, "in_sample_fmt", codecCtx_->sample_fmt, 0);
    
    // 设置输出参数
    av_opt_set_chlayout(swrCtx_, "out_chlayout", &codecCtx_->ch_layout, 0);
    // 根据播放速度调整采样率
    int outSampleRate = static_cast<int>(codecCtx_->sample_rate * speed_.load());
    av_opt_set_int(swrCtx_, "out_sample_rate", outSampleRate, 0);
    av_opt_set_sample_fmt(swrCtx_, "out_sample_fmt", codecCtx_->sample_fmt, 0);
    
    // 初始化重采样上下文
    int ret = swr_init(swrCtx_);
    if (ret < 0) {
        swr_free(&swrCtx_);
        swrCtx_ = nullptr;
        needResample_ = false;
        return false;
    }
    
    return true;
}

AVFrame *AudioDecoder::resampleFrame(AVFrame *frame)
{
    if (!needResample_ || !swrCtx_ || !frame) {
        return frame;
    }

    // 创建输出帧
    AVFrame *outFrame = av_frame_alloc();
    if (!outFrame) {
        return frame;
    }

    // 设置输出帧参数
    outFrame->format = frame->format;
    outFrame->ch_layout = frame->ch_layout;
    // 调整采样率
    outFrame->sample_rate = static_cast<int>(frame->sample_rate * speed_.load());
    // 调整采样数
    int outSamples = av_rescale_rnd(swr_get_delay(swrCtx_, frame->sample_rate) + frame->nb_samples, outFrame->sample_rate, frame->sample_rate, AV_ROUND_UP);
    
    // 分配输出缓冲区
    int ret = av_frame_get_buffer(outFrame, 0);
    if (ret < 0) {
        av_frame_free(&outFrame);
        return frame;
    }

    // 执行重采样
    ret = swr_convert(swrCtx_, outFrame->data, outSamples, (const uint8_t **)frame->data, frame->nb_samples);
    if (ret < 0) {
        av_frame_free(&outFrame);
        return frame;
    }

    // 设置输出帧参数
    outFrame->nb_samples = ret;

    // 复制时间戳
    outFrame->pts = frame->pts;
    outFrame->pkt_dts = frame->pkt_dts;

    // 释放输入帧
    av_frame_free(&frame);

    return outFrame;
}

AVMediaType AudioDecoder::type() const
{
    return AVMEDIA_TYPE_AUDIO;
}

double AudioDecoder::calculateFrameDisplayTime(double pts, double duration)
{
    if (std::isnan(pts)) {
        return 0.0;
    }
    
    double currentTime = av_gettime_relative() / 1000.0;
    
    // 首次调用，初始化
    if (utils::equal(lastFrameTime_, 0.0)) {
        lastFrameTime_ = currentTime;
        return 0.0;
    }
    
    // 获取当前播放速度
    float currentSpeed = speed_.load();
    if (currentSpeed <= 0.0f) {
        currentSpeed = 1.0f; // 防止除零错误
    }
    
    // 基于帧率计算理论帧间隔，并考虑播放速度
    double frameInterval = duration;
    frameInterval /= currentSpeed; // 速度越快，帧间隔越短
    
    // 计算下一帧应该解码的时间点
    double nextFrameTime = lastFrameTime_ + frameInterval;
    
    // 计算基本延迟时间
    double baseDelay = nextFrameTime - currentTime;
    
    // 如果有同步控制器，直接使用同步控制器计算的延迟
    double finalDelay = baseDelay;
    // if (syncController_) {
    //     // 传入播放速度到同步控制器
    //     finalDelay = syncController_->computeVideoDelay(pts, duration, currentSpeed) * 0.001;
    // }
    // if (!utils::greaterAndEqual(finalDelay, 0.0)) {
    //     finalDelay = 0.0;
    // }
    
    // 更新上一帧时间
    lastFrameTime_ = currentTime + finalDelay;

    // std::cout << "pts: " << pts << ", duration: " << duration << ", delay: " << finalDelay << std::endl; // Add this line to print ou
    
    return finalDelay;
}