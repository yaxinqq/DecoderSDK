extern "C" {
#include <libavutil/time.h>
#include "libavutil/opt.h"
}

#include "AudioDecoder.h"
#include "Logger.h"
#include "Utils.h"

namespace {
const std::string kAudioDecoderName = "Audio Decoder";
}

AudioDecoder::AudioDecoder(std::shared_ptr<Demuxer> demuxer,
                           std::shared_ptr<SyncController> syncController,
                           std::shared_ptr<EventDispatcher> eventDispatcher)
    : DecoderBase(demuxer, syncController, eventDispatcher),
      lastFrameTime_(std::nullopt)
{
}

AudioDecoder::~AudioDecoder()
{
    close();
    if (swrCtx_) {
        swr_free(&swrCtx_);
        swrCtx_ = nullptr;
    }
}

AVMediaType AudioDecoder::type() const
{
    return AVMEDIA_TYPE_AUDIO;
}

void AudioDecoder::decodeLoop()
{
    // 解码帧
    Frame frame;
    frame.ensureAllocated();
    if (!frame.isValid()) {
        LOG_ERROR("Audio Decoder decodeLoop error: Failed to allocate frame!");
        handleDecodeError(kAudioDecoderName, MediaType::kMediaTypeAudio,
                          AVERROR(ENOMEM), "Failed to allocate frame!");
    }

    auto packetQueue = demuxer_->packetQueue(type());
    if (!packetQueue) {
        LOG_ERROR(
            "Audio Decoder decodeLoop error: Can not find packet queue from "
            "demuxer!");
        handleDecodeError(kAudioDecoderName, MediaType::kMediaTypeAudio,
                          AVERROR_UNKNOWN,
                          "Can not find packet queue from "
                          "demuxer!");
        return;
    }

    int serial = packetQueue->serial();
    syncController_->updateVideoClock(0.0, serial);

    bool readFirstFrame = false;
    bool occuredError = false;
    double lastSpeed = speed_;

    resetStatistics();
    while (isRunning_) {
        // 检查序列号变化
        if (checkAndUpdateSerial(serial, packetQueue.get())) {
            // 序列号发生变化时，重置下列数据

            // 重置视频时钟
            syncController_->updateVideoClock(0.0, serial);
            // 重置最后帧时间
            lastFrameTime_ = std::nullopt;
        }

        // 获取一个可写入的帧
        Frame *outFrame = frameQueue_.getWritableFrame();
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

        // 发送包到解码器
        int ret = avcodec_send_packet(codecCtx_, packet.get());
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            continue;
        }

        // 接收解码帧
        ret = avcodec_receive_frame(codecCtx_, frame.get());
        if (ret < 0) {
            // 错误处理失败时，就continue，代表此时是EOF或是EAGAIN
            if (handleDecodeError(kAudioDecoderName, MediaType::kMediaTypeAudio,
                                  ret, "Decoder error: ")) {
                occuredError = true;
            }
            continue;
        }

        // 检查是否需要重新初始化重采样
        if (needResampleUpdate(lastSpeed)) {
            initResampleContext();
            lastSpeed = speed_;
        }

        // 重采样处理
        Frame outputFrame;
        if (needResample_ && swrCtx_) {
            outputFrame = resampleFrame(frame);
            if (!outputFrame.isValid()) {
                handleDecodeError(kAudioDecoderName, MediaType::kMediaTypeAudio,
                                  AVERROR_UNKNOWN, "Resample frame failed!");
                continue;
            }
        } else {
            outputFrame = frame;  // 只在需要时拷贝
        }

        // 计算帧持续时间(单位 s)
        const double duration =
            frame.nbSamples() / (double)codecCtx_->sample_rate;
        // 计算PTS（单位s）
        const double pts = calculatePts(frame);

        // 更新音频时钟
        if (!std::isnan(pts)) {
            syncController_->updateAudioClock(pts, serial);
        }

        // 如果当前小于seekPos，丢弃帧，先不加锁了
        if (!utils::greaterAndEqual(pts, seekPos_)) {
            frame.unref();
            continue;
        }

        // 如果是第一帧，发出事件
        if (!readFirstFrame) {
            readFirstFrame = true;
            handleFirstFrame(kAudioDecoderName, MediaType::kMediaTypeAudio);
        }

        // 如果恢复，则发出事件
        if (occuredError) {
            occuredError = false;
            handleDecodeRecovery(kAudioDecoderName, MediaType::kMediaTypeAudio);
        }

        // 将解码后的帧复制到输出帧
        *outFrame = std::move(outputFrame);
        outFrame->setSerial(serial);
        outFrame->setDurationByFps(duration);
        outFrame->setSecPts(pts);

        // 计算延时
        // 如果启用了帧率控制，则根据帧率控制推送速度
        if (isFrameRateControlEnabled()) {
            double displayTime = calculateFrameDisplayTime(
                pts, duration * 1000.0, lastFrameTime_);
            if (utils::greater(displayTime, 0.0)) {
                std::this_thread::sleep_until(lastFrameTime_.value());
            }
        }

        // 提交帧到队列
        frameQueue_.commitFrame();

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

bool AudioDecoder::initResampleContext()
{
    // 释放旧的重采样上下文
    if (swrCtx_) {
        swr_free(&swrCtx_);
        swrCtx_ = nullptr;
    }

    // 获得当前速度
    const double curSpeed = speed();

    // 如果播放速度接近1.0，不需要重采样
    if (std::abs(curSpeed - 1.0f) < 0.01f) {
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
    int outSampleRate = static_cast<int>(codecCtx_->sample_rate * curSpeed);
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

Frame AudioDecoder::resampleFrame(const Frame &frame)
{
    if (!needResample_ || !swrCtx_ || !frame.isValid()) {
        return frame;  // 返回原帧的拷贝（此处应该不会被触发，已在外部优化）
    }

    if (!resampleFrame_.isValid()) {
        resampleFrame_.ensureAllocated();
    }

    // 获得当前速度
    const double curSpeed = speed();

    const int inputSampleRate = frame.sampleRate();
    const int outputSampleRate = static_cast<int>(inputSampleRate * curSpeed);

    // 检查是否需要重新配置
    bool needReconfig = (resampleFrame_.sampleRate() != outputSampleRate) ||
                        (resampleFrame_.get()->ch_layout.nb_channels !=
                         frame.get()->ch_layout.nb_channels) ||
                        (resampleFrame_.get()->format != frame.get()->format);

    if (needReconfig) {
        // 重新配置输出帧参数
        resampleFrame_.setPixelFormat(
            static_cast<AVPixelFormat>(frame.get()->format));
        resampleFrame_.setChannelLayout(frame.channelLayout());
        resampleFrame_.setSampleRate(outputSampleRate);
    }

    // 计算输出采样数
    int outSamples = av_rescale_rnd(
        swr_get_delay(swrCtx_, frame.sampleRate()) + frame.get()->nb_samples,
        resampleFrame_.sampleRate(), frame.sampleRate(), AV_ROUND_UP);

    // 检查是否需要重新分配缓冲区
    bool needRealloc = needReconfig ||
                       (resampleFrame_.get()->nb_samples < outSamples) ||
                       !resampleFrame_.get()->data[0];

    if (needRealloc) {
        // 释放旧缓冲区
        av_frame_unref(resampleFrame_.get());

        // 设置新的采样数（分配足够的空间）
        resampleFrame_.setNbSamples(outSamples);

        // 分配新缓冲区
        int ret = av_frame_get_buffer(resampleFrame_.get(), 0);
        if (ret < 0) {
            return Frame();
        }
    }

    // 执行重采样
    int ret = swr_convert(swrCtx_, resampleFrame_.get()->data, outSamples,
                          (const uint8_t **)frame.get()->data,
                          frame.get()->nb_samples);
    if (ret < 0) {
        return Frame();
    }

    // 设置实际输出采样数和时间戳
    resampleFrame_.setNbSamples(ret);
    resampleFrame_.setAvPts(frame.avPts());
    resampleFrame_.setPktDts(frame.pktDts());

    // 使用移动语义返回，避免拷贝
    Frame result = std::move(resampleFrame_);
    resampleFrame_ = Frame();  // 重置复用帧
    return result;
}

bool AudioDecoder::needResampleUpdate(double lastSpeed)
{
    return std::abs(speed_ - lastSpeed) > 0.01f;
}