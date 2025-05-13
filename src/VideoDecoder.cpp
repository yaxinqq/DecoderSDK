#include "VideoDecoder.h"
#include <thread>
#include <chrono>

extern "C" {
#include <libavutil/time.h>
}

VideoDecoder::VideoDecoder(std::shared_ptr<Demuxer> demuxer)
    : DecoderBase(demuxer)
    , frameRate_(0.0)
    , frameRateControlEnabled_(true)
    , lastFrameTime_(0.0)
{
    // 初始化视频时钟
    clock_.init(-1);
}

VideoDecoder::~VideoDecoder() 
{
    close();
}

#include <iostream>
bool VideoDecoder::open()
{
    bool ret = DecoderBase::open();
    if (!ret)
        return false;

    // 创建硬件加速器（默认尝试自动选择最佳硬件加速方式）
    hwAccel_ = HardwareAccelFactory::getInstance().createHardwareAccel(HWAccelType::AUTO);
    
    if (hwAccel_) {
        if (hwAccel_->getType() == HWAccelType::NONE) {
            // Todo: log
            // std::cout << "没有找到可用的硬件加速器，将使用软解码" << std::endl;
        } else {
            // Todo: log
            // std::cout << "使用硬件加速器: " << hwAccel_->getDeviceName() 
            //           << " (" << hwAccel_->getDeviceDescription() << ")" << std::endl;
            
            // 设置解码器上下文使用硬件加速
            if (!hwAccel_->setupDecoder(codecCtx_)) {
                // Todo: log
                // std::cerr << "设置硬件加速失败，将回退到软解码" << std::endl;
                hwAccel_ = nullptr;
            }
        }
    }
    return true;
} 

void VideoDecoder::decodeLoop() {
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
        AVRational frame_rate = av_guess_frame_rate(demuxer_->formatContext(), stream_, NULL);
        
        // 更新视频帧率
        updateFrameRate(frame_rate);
        
        // 计算帧持续时间
        double duration = (frame_rate.num && frame_rate.den) 
            ? av_q2d(av_inv_q(frame_rate)) 
            : 0;
        
        // 计算PTS
        double pts = frame->pts != AV_NOPTS_VALUE ? frame->pts * av_q2d(tb) : NAN;
        
        // 更新视频时钟
        if (!std::isnan(pts)) {
            clock_.setClock(pts, serial);
        }
        
        // 将解码后的帧复制到输出帧
        *outFrame = Frame(frame);
        outFrame->setSerial(serial);
        outFrame->setDuration(duration);
        // outFrame->setPts(pts);
        
        // 检查是否是硬件加速解码
        outFrame->setIsInHardware(frame->hw_frames_ctx != NULL);
        
        // 如果启用了帧率控制，则根据帧率控制推送速度
        if (frameRateControlEnabled_ && frameRate_ > 0.0) {
            double displayTime = calculateFrameDisplayTime(pts, duration);
            if (displayTime > 0.0) {
                // 等待适当的时间再推送帧
                std::this_thread::sleep_for(std::chrono::microseconds(
                    static_cast<int64_t>(displayTime * 1000000)));
            }
        }
        
        // 推入帧队列
        frameQueue_.push();
    }
    
    av_frame_free(&frame);
}

AVMediaType VideoDecoder::type() const
{
    return AVMEDIA_TYPE_VIDEO;
}

void VideoDecoder::updateFrameRate(AVRational frameRate)
{
    if (frameRate.num && frameRate.den) {
        double newFrameRate = av_q2d(frameRate);
        
        // 如果帧率发生变化，更新帧率
        if (frameRate_ == 0.0 || std::fabs(frameRate_ - newFrameRate) > 0.1) {
            frameRate_ = newFrameRate;
        }
    }
}

double VideoDecoder::calculateFrameDisplayTime(double pts, double duration)
{
    if (std::isnan(pts)) {
        return 0.0;
    }
    
    double currentTime = av_gettime_relative() / 1000000.0;
    
    // 首次调用，初始化
    if (lastFrameTime_ == 0.0) {
        lastFrameTime_ = currentTime;
        return 0.0;
    }
    
    // 计算下一帧应该显示的时间
    double nextFrameTime = lastFrameTime_ + duration;
    double delay = nextFrameTime - currentTime;
    
    // 如果延迟为负，说明已经落后了，立即显示
    if (delay < 0.0) {
        delay = 0.0;
    }
    
    // 更新上一帧时间
    lastFrameTime_ = currentTime + delay;
    
    return delay;
}