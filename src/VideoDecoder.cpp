#include "VideoDecoder.h"
#include <thread>
#include <chrono>

extern "C" {
#include <libavutil/time.h>
}

VideoDecoder::VideoDecoder(std::shared_ptr<Demuxer> demuxer, std::shared_ptr<SyncController> syncController)
    : DecoderBase(demuxer)
    , syncController_(syncController)
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

bool VideoDecoder::open()
{
    bool ret = DecoderBase::open();
    if (!ret)
        return false;

    // 预测帧率
    AVRational tb = stream_->time_base;
    AVRational frame_rate = av_guess_frame_rate(demuxer_->formatContext(), stream_, NULL);
    
    // 更新视频帧率
    updateFrameRate(frame_rate);
    return true;
} 

#include <iostream>
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
    
    while (isRunning_.load())
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
        bool gotPacket = packetQueue->pop(packet, 1);
        
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
        
        // 计算帧持续时间
        AVRational tb = stream_->time_base;
        AVRational frame_rate = av_guess_frame_rate(demuxer_->formatContext(), stream_, NULL);
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
                // 用条件变量替换 sleep，进行线程休眠
                std::unique_lock<std::mutex> lock(sleepMutex_);
                if (sleepCond_.wait_for(lock, std::chrono::microseconds(static_cast<int64_t>(displayTime * 1000000)),
                                [this]{ return !isRunning_; })) {
                    // 被 stop 唤醒，直接退出
                    break;
                }
            }
        }
        
        // 推入帧队列
        frameQueue_.push();
    }
    
    av_frame_free(&frame);
}

bool VideoDecoder::setHardwareDecode()
{
    // 创建硬件加速器（默认尝试自动选择最佳硬件加速方式）
    hwAccel_ = HardwareAccelFactory::getInstance().createHardwareAccel(HWAccelType::AUTO);
        
    if (hwAccel_) {
        if (hwAccel_->getType() == HWAccelType::NONE) {
            // Todo: log
            // std::cout << "没有找到可用的硬件加速器，将使用软解码" << std::endl;
            hwAccel_.reset();
            return false;
        } else {
            // Todo: log
            // std::cout << "使用硬件加速器: " << hwAccel_->getDeviceName() 
            //           << " (" << hwAccel_->getDeviceDescription() << ")" << std::endl;
            
            // 设置解码器上下文使用硬件加速
            if (!hwAccel_->setupDecoder(codecCtx_)) {
                // Todo: log
                // std::cerr << "设置硬件加速失败，将回退到软解码" << std::endl;
                hwAccel_.reset();
                return false;
            }
        }
    }

    return true;
}

int VideoDecoder::calculateMaxPacketCount() const
{
    // 基础队列长度
    const int baseVideoCount = 25;
    
    // 根据速度和帧率计算，但设置上限
    int maxCount = std::min(
        static_cast<int>(frameRate_ * speed_), 
        baseVideoCount * 3
    );
    
    // 确保队列长度在合理范围内
    return std::clamp(maxCount, baseVideoCount, 120);
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

            // 更新包队列最大数量
            //demuxer_->packetQueue(type())->setMaxPacketCount(calculateMaxPacketCount());
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
    
    // 获取当前播放速度
    float currentSpeed = speed_.load();
    std::cout << "currentSpeed: " << currentSpeed << std::endl; // Add this line to print out the current speed
    if (currentSpeed <= 0.0f) {
        currentSpeed = 1.0f; // 防止除零错误
    }
    
    // 基于帧率计算理论帧间隔，并考虑播放速度
    double frameInterval = (frameRate_ > 0.0) ? (1.0 / frameRate_) : duration;
    frameInterval /= currentSpeed; // 速度越快，帧间隔越短
    
    // 计算下一帧应该解码的时间点
    double nextFrameTime = lastFrameTime_ + frameInterval;
    
    // 计算基本延迟时间
    double baseDelay = nextFrameTime - currentTime;
    
    // 如果有同步控制器，直接使用同步控制器计算的延迟
    double finalDelay = baseDelay;
    if (syncController_) {
        // 传入播放速度到同步控制器
        finalDelay = syncController_->computeVideoDelay(pts, duration, currentSpeed);
    }
    
    // 确保延迟不为负
    if (finalDelay < 0.0) {
        finalDelay = 0.0;
    }
    
    // 更新上一帧时间
    lastFrameTime_ = currentTime + finalDelay;

    std::cout << "pts: " << pts << ", duration: " << duration << ", delay: " << finalDelay << std::endl; // Add this line to print ou
    
    return finalDelay;
}