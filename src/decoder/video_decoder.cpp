#include "video_decoder.h"

#include <chrono>
#include <thread>

extern "C" {
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

#include "demuxer/demuxer.h"
#include "event_system/event_dispatcher.h"
#include "logger/Logger.h"
#include "stream_sync/stream_sync_manager.h"
#include "utils/common_utils.h"

namespace {
const std::string kVideoDecoderName = "Video Decoder";
}

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

VideoDecoder::VideoDecoder(std::shared_ptr<Demuxer> demuxer,
                           std::shared_ptr<StreamSyncManager> StreamSyncManager,
                           std::shared_ptr<EventDispatcher> eventDispatcher)
    : DecoderBase(demuxer, StreamSyncManager, eventDispatcher), frameRate_(0.0)
{
    init({});
}

VideoDecoder::~VideoDecoder()
{
    close();
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
}

void VideoDecoder::init(const Config &config)
{
    hwAccelType_ = config.hwAccelType;
    deviceIndex_ = config.hwDeviceIndex;
    softPixelFormat_ = utils::imageFormat2AVPixelFormat(config.swVideoOutFormat);
    requireFrameInMemory_ = config.requireFrameInSystemMemory;
    hwContextCallbeck_ = config.createHwContextCallback;
}

bool VideoDecoder::open()
{
    if (!DecoderBase::open()) {
        return false;
    }

    // 预测帧率
    AVRational frame_rate = av_guess_frame_rate(demuxer_->formatContext(), stream_, NULL);
    updateFrameRate(frame_rate);

    return true;
}

void VideoDecoder::decodeLoop()
{
    // 解码帧
    Frame frame;
    frame.ensureAllocated();
    if (!frame.isValid()) {
        LOG_ERROR("Video Decoder decodeLoop error: Failed to allocate frame!");
        handleDecodeError(kVideoDecoderName, MediaType::kMediaTypeVideo, AVERROR(ENOMEM),
                          "Failed to allocate frame!");
    }

    auto packetQueue = demuxer_->packetQueue(type());
    if (!packetQueue) {
        LOG_ERROR(
            "Video Decoder decodeLoop error: Can not find packet queue from "
            "demuxer!");
        handleDecodeError(kVideoDecoderName, MediaType::kMediaTypeVideo, AVERROR_UNKNOWN,
                          "Can not find packet queue from "
                          "demuxer!");
        return;
    }

    int serial = packetQueue->serial();
    syncController_->updateVideoClock(0.0, serial);

    bool hasKeyFrame = false;
    bool readFirstFrame = false;
    bool occuredError = false;

    resetStatistics();
    while (isRunning_.load()) {
        // 如果在等待预缓冲，则暂停解码
        if (waitingForPreBuffer_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 检查序列号变化
        if (checkAndUpdateSerial(serial, packetQueue.get())) {
            // 序列号发生变化时，重置下列数据
            // 重新等待关键帧
            hasKeyFrame = false;
            // 重置视频时钟
            syncController_->updateVideoClock(0.0, serial);
            // 重置最后帧时间
            lastFrameTime_ = std::nullopt;
        }

        // 获取一个可写入的帧
        Frame *outFrame = frameQueue_->getWritableFrame();
        if (!outFrame)
            break;

        // 从包队列中获取一个包
        Packet packet;
        bool gotPacket = packetQueue->pop(packet, 1);
        if (!gotPacket) {
            // 没有包可用，可能是队列为空或已中止
            if (packetQueue->isAborted())
                break;
            continue;
        }

        // 检查序列号
        if (packet.serial() != serial)
            continue;

        // 等待关键帧
        if (!hasKeyFrame && (packet.get()->flags & AV_PKT_FLAG_KEY) == 0) {
            continue;
        }
        hasKeyFrame = true;

        // 发送包到解码器
        int ret = avcodec_send_packet(codecCtx_, packet.get());
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            continue;
        }

        // 接收解码帧
        ret = avcodec_receive_frame(codecCtx_, frame.get());
        if (ret < 0) {
            // 错误处理失败时，就continue，代表此时是EOF或是EAGAIN
            if (handleDecodeError(kVideoDecoderName, MediaType::kMediaTypeVideo, ret,
                                  "Decoder error: ")) {
                occuredError = true;
            }
            continue;
        }

        // 计算帧持续时间(单位 s)
        const double duration = 1 / av_q2d(stream_->avg_frame_rate);
        // 计算PTS（单位s）
        const double pts = calculatePts(frame);

        // 更新视频时钟
        if (!std::isnan(pts)) {
            syncController_->updateVideoClock(pts, serial);
        }

        // 如果当前小于seekPos，丢弃帧，先不加锁了
        if (!utils::greaterAndEqual(pts, seekPos_)) {
            frame.unref();
            continue;
        }

        // 如果是第一帧，发出事件
        if (!readFirstFrame) {
            readFirstFrame = true;
            handleFirstFrame(kVideoDecoderName, MediaType::kMediaTypeVideo);
        }

        // 如果恢复，则发出事件
        if (occuredError) {
            occuredError = false;
            handleDecodeRecovery(kVideoDecoderName, MediaType::kMediaTypeVideo);
        }

        // 处理帧格式转换
        Frame outputFrame = processFrameConversion(frame);
        if (!outputFrame.isValid()) {
            continue;
        }

        // 将解码后的帧复制到输出帧
        *outFrame = std::move(outputFrame);
        outFrame->setSerial(serial);
        outFrame->setDurationByFps(duration);
        outFrame->setSecPts(pts);

        // 如果启用了帧率控制，则根据帧率控制推送速度
        if (isFrameRateControlEnabled()) {
            double displayTime = calculateFrameDisplayTime(pts, duration * 1000.0, lastFrameTime_);
            if (utils::greater(displayTime, 0.0)) {
                std::this_thread::sleep_until(lastFrameTime_.value());
            }
        }

        // 提交帧到队列
        frameQueue_->commitFrame();

        // 更新统计信息
        statistics_.framesDecoded.fetch_add(1);
        // 每到100帧，统计一次解码时间
        if (statistics_.framesDecoded.load() % 100 == 0) {
            updateTotalDecodeTime();
        }

        // 清理当前帧，准备下一次解码
        frame.unref();
    }

    // 循环结束时，统计一次解码时间
    updateTotalDecodeTime();
}

Frame VideoDecoder::processFrameConversion(const Frame &inputFrame)
{
    AVFrame *avFrame = inputFrame.get();
    if (!avFrame) {
        return Frame();
    }

    bool isHardwareFrame = (avFrame->hw_frames_ctx != nullptr);
    AVPixelFormat currentFormat = inputFrame.pixelFormat();

    // 早期退出：如果不需要任何转换
    if (!isHardwareFrame && !requireFrameInMemory_ && currentFormat == softPixelFormat_) {
        return inputFrame; // 直接返回，最高效
    }

    // 硬件帧处理
    if (isHardwareFrame && requireFrameInMemory_) {
        Frame memoryFrame = transferHardwareFrame(inputFrame);
        if (!memoryFrame.isValid()) {
            return Frame();
        }

        // 检查是否还需要格式转换
        if (memoryFrame.pixelFormat() != softPixelFormat_) {
            return convertSoftwareFrame(memoryFrame);
        }

        return memoryFrame;
    }

    // 软件帧格式转换
    if (!isHardwareFrame && currentFormat != softPixelFormat_) {
        return convertSoftwareFrame(inputFrame);
    }

    // 默认返回原帧
    return inputFrame;
}

Frame VideoDecoder::transferHardwareFrame(const Frame &hwFrame)
{
    if (!memoryFrame_.isValid()) {
        memoryFrame_.ensureAllocated();
    }

    if (!hwAccel_->transferFrameToHost(hwFrame.get(), memoryFrame_.get())) {
        handleDecodeError(kVideoDecoderName, MediaType::kMediaTypeVideo, AVERROR_UNKNOWN,
                          "TransferFrameToHost failed!");
        return Frame();
    }

    // 创建新Frame并移动，避免拷贝
    Frame result = std::move(memoryFrame_);
    memoryFrame_ = Frame(); // 重置为空，下次会重新分配
    return result;
}

Frame VideoDecoder::convertSoftwareFrame(const Frame &frame)
{
    if (!swsFrame_.isValid()) {
        swsFrame_.ensureAllocated();
    }

    // 初始化转换上下文
    swsCtx_ = sws_getCachedContext(swsCtx_, frame.width(), frame.height(), frame.pixelFormat(),
                                   frame.width(), frame.height(), softPixelFormat_, SWS_BILINEAR,
                                   nullptr, nullptr, nullptr);

    if (!swsCtx_) {
        handleDecodeError(kVideoDecoderName, MediaType::kMediaTypeVideo, AVERROR_UNKNOWN,
                          "SwsContext alloc failed!");
        return Frame();
    }

    // 设置目标帧参数
    swsFrame_.setPixelFormat(softPixelFormat_);
    swsFrame_.setWidth(frame.width());
    swsFrame_.setHeight(frame.height());
    swsFrame_.setAvPts(frame.avPts());

    // 分配缓冲区
    int ret = av_frame_get_buffer(swsFrame_.get(), 0);
    if (ret < 0) {
        handleDecodeError(kVideoDecoderName, MediaType::kMediaTypeVideo, AVERROR_UNKNOWN,
                          "Frame buffer alloc failed!");
        return Frame();
    }

    // 执行转换
    ret = sws_scale(swsCtx_, (const uint8_t *const *)frame.get()->data, frame.get()->linesize, 0,
                    frame.height(), swsFrame_.get()->data, swsFrame_.get()->linesize);

    if (ret <= 0) {
        handleDecodeError(kVideoDecoderName, MediaType::kMediaTypeVideo, AVERROR_UNKNOWN,
                          "SwsContext scale failed!");
        return Frame();
    }

    // 复制帧属性
    av_frame_copy_props(swsFrame_.get(), frame.get());

    // 创建新Frame并移动，避免拷贝
    Frame result = std::move(swsFrame_);
    swsFrame_ = Frame(); // 重置为空，下次会重新分配
    return result;
}

bool VideoDecoder::setupHardwareDecode()
{
    // 创建硬件加速器（默认尝试自动选择最佳硬件加速方式）
    hwAccel_ = HardwareAccelFactory::getInstance().createHardwareAccel(hwAccelType_, deviceIndex_, hwContextCallbeck_);
    if (!hwAccel_) {
        LOG_WARN("Hardware acceleration not available, using software decode");
        return false;
    }

    if (hwAccel_->getType() == HWAccelType::kNone) {
        LOG_WARN("Hardware acceleration not available, using software decode");
        hwAccel_.reset();
        return false;
    } else {
        LOG_INFO("Using hardware accelerator: {} ({}), device index: {}", hwAccel_->getDeviceName(),
                  hwAccel_->getDeviceDescription(), std::to_string(hwAccel_->getDeviceIndex()));

        // 设置解码器上下文使用硬件加速
        if (!hwAccel_->setupDecoder(codecCtx_)) {
            LOG_WARN("Hardware acceleration setup failed, falling back to software");
            hwAccel_.reset();
            return false;
        }
    }

    return true;
}

AVMediaType VideoDecoder::type() const
{
    return AVMEDIA_TYPE_VIDEO;
}

void VideoDecoder::requireFrameInSystemMemory(bool required)
{
    requireFrameInMemory_ = required;
}

double VideoDecoder::getFrameRate() const
{
    return frameRate_;
}

void VideoDecoder::updateFrameRate(AVRational frameRate)
{
    if (frameRate.num == 0 || frameRate.den == 0) {
        return;
    }

    double newFrameRate = av_q2d(frameRate);
    if (utils::equal(frameRate_, newFrameRate)) {
        return;
    }

    frameRate_ = newFrameRate;
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END