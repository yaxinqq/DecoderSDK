#include "Utils.h"
#include "VideoDecoder.h"
#include <thread>
#include <chrono>

extern "C" {
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

VideoDecoder::VideoDecoder(std::shared_ptr<Demuxer> demuxer, std::shared_ptr<SyncController> syncController)
    : DecoderBase(demuxer, syncController)
    , frameRate_(0.0)
    , lastFrameTime_(std::nullopt)
{
    // 先初始化默认硬件类型
    init();
}

VideoDecoder::~VideoDecoder() 
{
    close();
}

void VideoDecoder::init(HWAccelType type, int deviceIndex, AVPixelFormat softPixelFormat)
{
    hwAccelType_ = type;
    deviceIndex_ = deviceIndex;
    softPixelFormat_ = softPixelFormat;
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
    AVFrame* swsFrame = nullptr;  // 用于格式转换的帧
    struct SwsContext* swsCtx = nullptr;  // 格式转换上下文
    
    if (!frame)
        return;
    
    auto packetQueue = demuxer_->packetQueue(type());
    if (!packetQueue)
    {
        av_frame_free(&frame);
        return;
    }
    
    int serial = packetQueue->serial();
    syncController_->updateVideoClock(0.0, serial);
    
    while (isRunning_.load())
    {
        // 检查序列号是否变化
        if (serial != packetQueue->serial()) {
            avcodec_flush_buffers(codecCtx_);
            serial = packetQueue->serial();
            frameQueue_.setSerial(serial);

            // 重置视频时钟
            syncController_->updateVideoClock(0.0, serial);
            // 重置最后帧时间
            lastFrameTime_ = std::nullopt;
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
        
        // 计算帧持续时间(单位 s)
        const double duration = 1 / av_q2d(stream_->avg_frame_rate);
        // 计算PTS（单位s）
        const double pts = calculatePts(frame);
        
        // 更新视频时钟
        if (!std::isnan(pts)) {
            syncController_->updateVideoClock(pts, serial);
        }

		// 如果当前小于seekPos，丢弃帧
		if (!utils::greaterAndEqual(pts, seekPos_.load())) {
			av_frame_unref(frame);
			continue;
		}
        
        // 软解时进行图像格式转换
        AVFrame* outputFrame = frame;
        if (!frame->hw_frames_ctx && frame->format != softPixelFormat_) {
            // 如果是软解且格式不匹配，进行格式转换
            if (!swsFrame) {
                swsFrame = av_frame_alloc();
                if (!swsFrame) {
                    av_frame_unref(frame);
                    continue;
                }
            }
            
            // 初始化转换上下文
            swsCtx = sws_getCachedContext(swsCtx,
                frame->width, frame->height, (AVPixelFormat)frame->format,
                frame->width, frame->height, softPixelFormat_,
                SWS_BILINEAR, NULL, NULL, NULL);
                
            if (!swsCtx) {
                av_frame_unref(frame);
                continue;
            }
            
            // 设置目标帧参数
            swsFrame->format = softPixelFormat_;
            swsFrame->width = frame->width;
            swsFrame->height = frame->height;
            swsFrame->pts = frame->pts;
            
            // 分配缓冲区
            ret = av_frame_get_buffer(swsFrame, 0);
            if (ret < 0) {
                av_frame_unref(frame);
                continue;
            }
            
            // 确保帧可写
            ret = av_frame_make_writable(swsFrame);
            if (ret < 0) {
                av_frame_unref(frame);
                continue;
            }
            
            // 执行格式转换
            ret = sws_scale(swsCtx, 
                (const uint8_t* const*)frame->data, frame->linesize, 0, frame->height,
                swsFrame->data, swsFrame->linesize);
                
            if (ret <= 0) {
                av_frame_unref(frame);
                continue;
            }
            
            // 使用转换后的帧
            outputFrame = swsFrame;
        }
        
        // 将解码后的帧复制到输出帧
        *outFrame = Frame(outputFrame);
        outFrame->setSerial(serial);
        outFrame->setDuration(duration);
        outFrame->setPts(pts);
        
        // 检查是否是硬件加速解码
        outFrame->setIsInHardware(frame->hw_frames_ctx != NULL);
        
        // 如果使用了转换帧，需要释放原始帧
        if (outputFrame == swsFrame) {
            av_frame_unref(frame);
        }
        
        // 如果启用了帧率控制，则根据帧率控制推送速度
        if (frameRateControlEnabled_) {
             double displayTime = calculateFrameDisplayTime(pts, duration * 1000.0);
			 if (utils::greater(displayTime, 0.0)) {
                 std::this_thread::sleep_until(lastFrameTime_.value());
			 }
            //std::this_thread::sleep_until(nextFrameTime_.value());
        }
        
        // 推入帧队列
        frameQueue_.push();
    }
    
    // 清理资源
    if (swsCtx) {
        sws_freeContext(swsCtx);
    }
    if (swsFrame) {
        av_frame_free(&swsFrame);
    }
    av_frame_free(&frame);
}

bool VideoDecoder::setHardwareDecode()
{
    // 创建硬件加速器（默认尝试自动选择最佳硬件加速方式）
    hwAccel_ = HardwareAccelFactory::getInstance().createHardwareAccel(hwAccelType_, deviceIndex_);
        
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
    
    // 获取当前播放速度
    float currentSpeed = speed_.load();
    if (currentSpeed <= 0.0f) {
        currentSpeed = 1.0f; // 防止除零错误
    }

    // 首次调用，初始化
    auto currentTime = std::chrono::steady_clock::now();
    if (!lastFrameTime_.has_value()) {
        lastFrameTime_ = currentTime;
        return 0.0;
    }
    
    // 基于帧率计算理论帧间隔，并考虑播放速度
    double frameInterval = duration;
    frameInterval /= currentSpeed; // 速度越快，帧间隔越短
    
    // 计算下一帧应该解码的时间点
    const auto nextFrameTime = *lastFrameTime_ + std::chrono::microseconds(static_cast<int64_t>(frameInterval * 1000.0));
    
    // 计算基本延迟时间
    double baseDelay = std::chrono::duration_cast<std::chrono::microseconds>(nextFrameTime - currentTime).count() / 1000.0;
    
    // 如果有同步控制器，直接使用同步控制器计算的延迟
    double finalDelay = baseDelay;
    if (syncController_->master() != SyncController::MasterClock::Video) {
        finalDelay = syncController_->computeVideoDelay(pts, duration, baseDelay, currentSpeed);
    }
    
    // 更新上一帧时间
    lastFrameTime_ = currentTime + std::chrono::microseconds(static_cast<int64_t>(finalDelay * 1000.0));

    // std::cout << "pts: " << pts << ", duration: " << duration << ", delay: " << finalDelay << std::endl; // Add this line to print ou
    
    return finalDelay;
}