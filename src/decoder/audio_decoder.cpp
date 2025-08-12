#include "audio_decoder.h"

extern "C" {
#include <libavutil/time.h>
#include "libavutil/opt.h"
}

#include "demuxer/demuxer.h"
#include "event_system/event_dispatcher.h"
#include "logger/logger.h"
#include "stream_sync/stream_sync_manager.h"
#include "utils/common_utils.h"

namespace {
const std::string kAudioDecoderName = "Audio Decoder";
}

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

AudioDecoder::AudioDecoder(std::shared_ptr<Demuxer> demuxer,
                           std::shared_ptr<StreamSyncManager> StreamSyncManager,
                           std::shared_ptr<EventDispatcher> eventDispatcher)
    : DecoderBase(demuxer, StreamSyncManager, eventDispatcher)
{
    init({});
}

AudioDecoder::~AudioDecoder()
{
    close();
    cleanupResampleResources();
    cleanupFormatConvertResources();
}

void AudioDecoder::init(const Config &config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    audioInterleaved_ = config.audioInterleaved;
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
        handleDecodeError(kAudioDecoderName, MediaType::kMediaTypeAudio, AVERROR(ENOMEM),
                          "Failed to allocate frame!");
    }

    auto packetQueue = demuxer_->packetQueue(type());
    if (!packetQueue) {
        LOG_ERROR(
            "Audio Decoder decodeLoop error: Can not find packet queue from "
            "demuxer!");
        handleDecodeError(kAudioDecoderName, MediaType::kMediaTypeAudio, AVERROR_UNKNOWN,
                          "Can not find packet queue from "
                          "demuxer!");
        return;
    }

    int serial = packetQueue->serial();
    syncController_->updateAudioClock(0.0, serial);

    bool readFirstFrame = false;
    bool occuredError = false;
    double lastSpeed = speed_;

    resetStatistics();

    // 如果是实时流，此时应该清空包队列
    if (demuxer_->isRealTime()) {
        packetQueue->flush();
    }
    while (!requestInterruption_.load()) {
        // 如果在等待预缓冲，则暂停解码
        if (waitingForPreBuffer_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        // 处理暂停状态
        if (isPaused_.load()) {
            std::unique_lock<std::mutex> lock(pauseMutex_);
            pauseCv_.wait(lock,
                          [this] { return !isPaused_.load() || requestInterruption_.load(); });
            if (requestInterruption_.load()) {
                break;
            }
            // 重置最后帧时间
            lastFrameTime_ = std::nullopt;
            continue;
        }

        // 检查序列号变化
        if (checkAndUpdateSerial(serial, packetQueue.get())) {
            // 序列号发生变化时，重置下列数据

            // 重置音频时钟
            syncController_->updateAudioClock(0.0, serial);
            // 重置最后帧时间
            lastFrameTime_ = std::nullopt;

            // 结束seeking状态
            utils::atomicUpdateIfNotEqual<bool>(demuxerSeeking_, false);
        }

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
            // 记录出错的信息
            LOG_WARN("{} send packet error, error code: {}, error string: {}", demuxer_->url(), ret,
                     utils::avErr2Str(ret));
            continue;
        }

        // 循环接收所有可能的解码帧
        while (true) {
            ret = avcodec_receive_frame(codecCtx_, frame.get());
            if (ret < 0) {
                if (ret == AVERROR(EAGAIN)) {
                    // 需要更多输入数据，跳出内层循环继续读取packet
                    break;
                } else {
                    // 其他错误（如EOF），处理错误
                    if (handleDecodeError(kAudioDecoderName, MediaType::kMediaTypeAudio, ret,
                                          "Decoder error: ")) {
                        occuredError = true;
                    }
                    break;
                }
            }
            const auto currentTime = std::chrono::high_resolution_clock::now();

            // 成功接收到一帧，进行处理
            // 检查是否需要重新初始化重采样
            if (needResampleUpdate(lastSpeed)) {
                initResampleContext();
                lastSpeed = speed_;
            }

            // 重采样处理
            Frame outputFrame;
            if (needResample_ && swrCtx_) {
                ret = 0;
                outputFrame = resampleFrame(frame, ret);
                if (!outputFrame.isValid()) {
                    handleDecodeError(kAudioDecoderName, MediaType::kMediaTypeAudio, ret,
                                      "Resample frame failed!");
                    frame.unref();
                    continue;
                }
            } else {
                outputFrame = frame; // 只在需要时拷贝
            }

            // 计算帧持续时间(单位 s)
            // 计算帧持续时间应该使用实际的采样率
            double actualSampleRate =
                needResample_ ? (codecCtx_->sample_rate * speed()) : codecCtx_->sample_rate;
            const double duration = outputFrame.nbSamples() / actualSampleRate;
            // 计算PTS（单位s）
            const double pts = calculatePts(outputFrame);
            if (!std::isnan(pts)) {
                syncController_->updateAudioClock(pts, serial);
            }

            // 如果当前小于seekPos，丢弃帧
            {
                // 如果当前正在seeking，直接跳过
                if (demuxerSeeking_.load()) {
                    frame.unref();
                    continue;
                }

                const auto targetPos = seekPos();
                if (utils::greater(targetPos, 0)) {
                    if (!utils::greaterAndEqual(pts, targetPos)) {
                        frame.unref();
                        continue;
                    }
                    utils::atomicUpdateIfNotEqual<int64_t>(seekPosMs_, -1);
                }
            }

            // 转换交错格式
            // 根据配置决定音频数据的存储方式
            if (!audioInterleaved_ && av_sample_fmt_is_planar(static_cast<AVSampleFormat>(
                                          outputFrame.get()->format)) == 0) {
                // 配置要求非交错，但当前是交错格式，转换为平面格式
                AVSampleFormat currentFormat =
                    static_cast<AVSampleFormat>(outputFrame.get()->format);
                AVSampleFormat targetFormat = AV_SAMPLE_FMT_NONE;

                // 获取对应的平面格式
                switch (currentFormat) {
                    case AV_SAMPLE_FMT_U8:
                        targetFormat = AV_SAMPLE_FMT_U8P;
                        break;
                    case AV_SAMPLE_FMT_S16:
                        targetFormat = AV_SAMPLE_FMT_S16P;
                        break;
                    case AV_SAMPLE_FMT_S32:
                        targetFormat = AV_SAMPLE_FMT_S32P;
                        break;
                    case AV_SAMPLE_FMT_FLT:
                        targetFormat = AV_SAMPLE_FMT_FLTP;
                        break;
                    case AV_SAMPLE_FMT_DBL:
                        targetFormat = AV_SAMPLE_FMT_DBLP;
                        break;
                    case AV_SAMPLE_FMT_S64:
                        targetFormat = AV_SAMPLE_FMT_S64P;
                        break;
                    default:
                        LOG_WARN("Unsupported audio format for planar conversion: {}",
                                 static_cast<int>(currentFormat));
                        break;
                }

                if (targetFormat != AV_SAMPLE_FMT_NONE) {
                    if (!convertAudioFormat(outputFrame, targetFormat)) {
                        LOG_WARN("Failed to convert audio to planar format");
                    }
                }
            } else if (audioInterleaved_ && av_sample_fmt_is_planar(static_cast<AVSampleFormat>(
                                                outputFrame.get()->format)) == 1) {
                // 配置要求交错，但当前是平面格式，转换为交错格式
                AVSampleFormat currentFormat =
                    static_cast<AVSampleFormat>(outputFrame.get()->format);
                AVSampleFormat targetFormat = AV_SAMPLE_FMT_NONE;

                // 获取对应的交错格式
                switch (currentFormat) {
                    case AV_SAMPLE_FMT_U8P:
                        targetFormat = AV_SAMPLE_FMT_U8;
                        break;
                    case AV_SAMPLE_FMT_S16P:
                        targetFormat = AV_SAMPLE_FMT_S16;
                        break;
                    case AV_SAMPLE_FMT_S32P:
                        targetFormat = AV_SAMPLE_FMT_S32;
                        break;
                    case AV_SAMPLE_FMT_FLTP:
                        targetFormat = AV_SAMPLE_FMT_FLT;
                        break;
                    case AV_SAMPLE_FMT_DBLP:
                        targetFormat = AV_SAMPLE_FMT_DBL;
                        break;
                    case AV_SAMPLE_FMT_S64P:
                        targetFormat = AV_SAMPLE_FMT_S64;
                        break;
                    default:
                        LOG_WARN("Unsupported audio format for interleaved conversion: {}",
                                 static_cast<int>(currentFormat));
                        break;
                }

                if (targetFormat != AV_SAMPLE_FMT_NONE) {
                    if (!convertAudioFormat(outputFrame, targetFormat)) {
                        LOG_WARN("Failed to convert audio to interleaved format");
                    }
                }
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

            // 获取一个可写入的帧
            Frame *outFrame = frameQueue_->getWritableFrame();
            if (!outFrame) {
                frame.unref();
                break; // 队列满了，退出
            }

            // 将解码后的帧复制到输出帧
            *outFrame = std::move(outputFrame);
            outFrame->setSerial(serial);
            outFrame->setDurationByFps(duration);
            outFrame->setSecPts(pts);
            outFrame->setMediaType(AVMEDIA_TYPE_AUDIO);

            // 计算延时
            // 如果启用了帧率控制，则根据帧率控制推送速度
            if (isFrameRateControlEnabled()) {
                // 计算基本延迟
                double baseDelay =
                    calculateFrameDisplayTime(pts, duration * 1000.0, currentTime, lastFrameTime_);

                // // 计算音频缓冲区延迟
                // double bufferDelay = frameQueue_->size() * duration * 1000.0; //
                // 估算缓冲区中的音频时长

                // // 使用同步控制器计算实际延迟
                // double syncDelay = syncController_->computeAudioDelay(pts, baseDelay, speed());

                // // 修正：限制最大延迟，防止延迟累积
                // const double maxDelay = 100.0; // 最大延迟100ms
                // syncDelay = std::min(syncDelay, maxDelay);

                // 使用同步后的延迟
                if (utils::greater(baseDelay, 0.0)) {
                    std::this_thread::sleep_until(
                        currentTime +
                        std::chrono::milliseconds(static_cast<int64_t>(baseDelay)));
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

    // FFmpeg版本兼容性处理
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100) // FFmpeg 5.1+
    // 设置输入参数 (新版本API)
    av_opt_set_chlayout(swrCtx_, "in_chlayout", &codecCtx_->ch_layout, 0);
    av_opt_set_int(swrCtx_, "in_sample_rate", codecCtx_->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx_, "in_sample_fmt", codecCtx_->sample_fmt, 0);

    // 设置输出参数 (新版本API)
    av_opt_set_chlayout(swrCtx_, "out_chlayout", &codecCtx_->ch_layout, 0);
#else
    // 设置输入参数 (旧版本API - FFmpeg 4.4.2)
    av_opt_set_int(swrCtx_, "in_channel_layout", codecCtx_->channel_layout, 0);
    av_opt_set_int(swrCtx_, "in_channels", codecCtx_->channels, 0);
    av_opt_set_int(swrCtx_, "in_sample_rate", codecCtx_->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx_, "in_sample_fmt", codecCtx_->sample_fmt, 0);

    // 设置输出参数 (旧版本API - FFmpeg 4.4.2)
    av_opt_set_int(swrCtx_, "out_channel_layout", codecCtx_->channel_layout, 0);
    av_opt_set_int(swrCtx_, "out_channels", codecCtx_->channels, 0);
#endif

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

Frame AudioDecoder::resampleFrame(const Frame &frame, int &errorCode)
{
    errorCode = 0;

    if (!needResample_ || !swrCtx_ || !frame.isValid()) {
        return frame;
    }

    // 获得当前速度
    const double curSpeed = speed();
    const int inputSampleRate = frame.sampleRate();
    const int outputSampleRate = static_cast<int>(inputSampleRate * curSpeed);

    // 检查是否需要重新配置 - 兼容不同FFmpeg版本
    bool needReconfig = false;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100) // FFmpeg 5.1+
    needReconfig =
        !resampleFrame_.isValid() || (resampleFrame_.sampleRate() != outputSampleRate) ||
        (resampleFrame_.get()->ch_layout.nb_channels != frame.get()->ch_layout.nb_channels) ||
        (resampleFrame_.get()->format != frame.get()->format);
#else
    needReconfig = !resampleFrame_.isValid() || (resampleFrame_.sampleRate() != outputSampleRate) ||
                   (resampleFrame_.get()->channels != frame.get()->channels) ||
                   (resampleFrame_.get()->format != frame.get()->format);
#endif

    if (needReconfig) {
        // 确保帧已分配
        if (!resampleFrame_.isValid()) {
            resampleFrame_.ensureAllocated();
        }

        // 重新配置输出帧参数
        resampleFrame_.setSampleFormat(static_cast<AVSampleFormat>(frame.get()->format));
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100) // FFmpeg 5.1+
        resampleFrame_.setChannelLayout(frame.channelLayout());
#else
        // FFmpeg 4.4.2使用旧的channel layout API
        resampleFrame_.get()->channel_layout = frame.get()->channel_layout;
        resampleFrame_.get()->channels = frame.get()->channels;
#endif
        resampleFrame_.setSampleRate(outputSampleRate);
    }

    // 计算输出采样数 - 添加安全检查
    int64_t delay = swr_get_delay(swrCtx_, frame.sampleRate());
    if (delay < 0) {
        delay = 0;
    }

    int64_t outSamples = av_rescale_rnd(delay + frame.get()->nb_samples, outputSampleRate,
                                        inputSampleRate, AV_ROUND_UP);

    // 添加合理的上限检查，防止异常大的缓冲区分配
    const int64_t maxSamples = frame.get()->nb_samples * 4; // 最多4倍的输入采样数
    if (outSamples > maxSamples) {
        outSamples = maxSamples;
    }

    if (outSamples <= 0) {
        errorCode = AVERROR(EINVAL);
        return Frame();
    }

    // 检查是否需要重新分配缓冲区
    bool needRealloc = needReconfig || (resampleFrame_.get()->nb_samples < outSamples) ||
                       !resampleFrame_.get()->data[0];

    if (needRealloc) {
        // 释放旧缓冲区
        av_frame_unref(resampleFrame_.get());

        // *** 修复：重新设置所有必要的帧参数 ***
        resampleFrame_.setSampleFormat(static_cast<AVSampleFormat>(frame.get()->format));
        resampleFrame_.setSampleRate(outputSampleRate);
        resampleFrame_.setNbSamples(static_cast<int>(outSamples));

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100) // FFmpeg 5.1+
        resampleFrame_.setChannelLayout(frame.channelLayout());
#else
        // FFmpeg 4.4.2使用旧的channel layout API
        resampleFrame_.get()->channel_layout = frame.get()->channel_layout;
        resampleFrame_.get()->channels = frame.get()->channels;
#endif

        // 分配新缓冲区
        errorCode = av_frame_get_buffer(resampleFrame_.get(), 0);
        if (errorCode < 0) {
            return Frame();
        }
    }

    // 执行重采样
    int ret = swr_convert(swrCtx_, resampleFrame_.get()->data, static_cast<int>(outSamples),
                          (const uint8_t **)frame.get()->data, frame.get()->nb_samples);
    if (ret < 0) {
        errorCode = ret;
        return Frame();
    }

    // 设置实际输出采样数和时间戳
    resampleFrame_.setNbSamples(ret);
    resampleFrame_.setAvPts(frame.avPts());
    resampleFrame_.setPktDts(frame.pktDts());

    // 复制其他重要属性
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    resampleFrame_.setTimeBase(frame.timeBase());
#endif

    // 修正duration计算逻辑
    if (frame.durationByFps() > 0) {
        // 按比例调整duration（保持为double类型）
        double newDuration = frame.durationByFps() * outputSampleRate / inputSampleRate;
        resampleFrame_.setDurationByFps(newDuration);
    }

    // 返回拷贝而不是移动，保持帧复用
    return resampleFrame_;
}

bool AudioDecoder::needResampleUpdate(double lastSpeed)
{
    return std::abs(speed_ - lastSpeed) > 0.01f;
}

bool AudioDecoder::initFormatConvertContext(AVSampleFormat srcFormat, AVSampleFormat dstFormat,
                                            int sampleRate, int channels, uint64_t channelLayout)
{
    // 检查参数是否与上次相同，如果相同则不需要重新初始化
    if (formatConvertCtx_ && lastSrcFormat_ == srcFormat && lastDstFormat_ == dstFormat &&
        lastConvertSampleRate_ == sampleRate && lastConvertChannels_ == channels &&
        lastConvertChannelLayout_ == channelLayout) {
        return true; // 复用现有上下文
    }

    // 释放旧的格式转换上下文
    if (formatConvertCtx_) {
        swr_free(&formatConvertCtx_);
        formatConvertCtx_ = nullptr;
    }

    // 创建新的格式转换上下文
    formatConvertCtx_ = swr_alloc();
    if (!formatConvertCtx_) {
        return false;
    }

    // FFmpeg版本兼容性处理
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100) // FFmpeg 5.1+
    // 设置输入参数 (新版本API)
    AVChannelLayout chLayout;
    av_channel_layout_from_mask(&chLayout, channelLayout);
    av_opt_set_chlayout(formatConvertCtx_, "in_chlayout", &chLayout, 0);
    av_opt_set_int(formatConvertCtx_, "in_sample_rate", sampleRate, 0);
    av_opt_set_sample_fmt(formatConvertCtx_, "in_sample_fmt", srcFormat, 0);

    // 设置输出参数 (新版本API)
    av_opt_set_chlayout(formatConvertCtx_, "out_chlayout", &chLayout, 0);
    av_opt_set_int(formatConvertCtx_, "out_sample_rate", sampleRate, 0);
    av_opt_set_sample_fmt(formatConvertCtx_, "out_sample_fmt", dstFormat, 0);
#else
    // 设置输入参数 (旧版本API - FFmpeg 4.4.2)
    av_opt_set_int(formatConvertCtx_, "in_channel_layout", channelLayout, 0);
    av_opt_set_int(formatConvertCtx_, "in_channels", channels, 0);
    av_opt_set_int(formatConvertCtx_, "in_sample_rate", sampleRate, 0);
    av_opt_set_sample_fmt(formatConvertCtx_, "in_sample_fmt", srcFormat, 0);

    // 设置输出参数 (旧版本API - FFmpeg 4.4.2)
    av_opt_set_int(formatConvertCtx_, "out_channel_layout", channelLayout, 0);
    av_opt_set_int(formatConvertCtx_, "out_channels", channels, 0);
    av_opt_set_int(formatConvertCtx_, "out_sample_rate", sampleRate, 0);
    av_opt_set_sample_fmt(formatConvertCtx_, "out_sample_fmt", dstFormat, 0);
#endif

    // 初始化格式转换上下文
    int ret = swr_init(formatConvertCtx_);
    if (ret < 0) {
        swr_free(&formatConvertCtx_);
        formatConvertCtx_ = nullptr;
        return false;
    }

    // 缓存参数，用于下次比较
    lastSrcFormat_ = srcFormat;
    lastDstFormat_ = dstFormat;
    lastConvertSampleRate_ = sampleRate;
    lastConvertChannels_ = channels;
    lastConvertChannelLayout_ = channelLayout;

    return true;
}

bool AudioDecoder::convertAudioFormat(Frame &frame, AVSampleFormat targetFormat)
{
    AVFrame *avFrame = frame.get();
    if (!avFrame || static_cast<AVSampleFormat>(avFrame->format) == targetFormat) {
        return true; // 已经是目标格式
    }

    const AVSampleFormat srcFormat = static_cast<AVSampleFormat>(avFrame->format);
    const int sampleRate = frame.sampleRate();
#if LIBAVUTIL_VERSION_MAJOR >= 57
    const int channels = frame.channelLayout().nb_channels;
#else
    const int channels = frame.channels();
#endif

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    const uint64_t channelLayout = avFrame->ch_layout.u.mask;
#else
    const uint64_t channelLayout = avFrame->channel_layout;
#endif

    // 初始化或复用格式转换上下文
    if (!initFormatConvertContext(srcFormat, targetFormat, sampleRate, channels, channelLayout)) {
        return false;
    }

    // 准备输出帧
    if (!formatConvertFrame_.isValid()) {
        formatConvertFrame_.ensureAllocated();
        if (!formatConvertFrame_.isValid()) {
            return false;
        }
    }

    // 检查是否需要重新配置输出帧
    bool needReconfig = (formatConvertFrame_.get()->format != targetFormat) ||
                        (formatConvertFrame_.sampleRate() != sampleRate) ||
#if LIBAVUTIL_VERSION_MAJOR >= 57
                        formatConvertFrame_.channelLayout().nb_channels != channels;
#else
                        formatConvertFrame_.channels() != channels;
#endif

    if (needReconfig) {
        // 重新配置输出帧参数
        av_frame_unref(formatConvertFrame_.get());
        formatConvertFrame_.get()->format = targetFormat;
        formatConvertFrame_.setSampleRate(sampleRate);

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        formatConvertFrame_.get()->ch_layout = avFrame->ch_layout;
#else
        formatConvertFrame_.get()->channel_layout = channelLayout;
        formatConvertFrame_.get()->channels = channels;
#endif
    }

    // 计算输出采样数（格式转换不改变采样数）
    const int outSamples = avFrame->nb_samples;

    // 检查是否需要重新分配缓冲区
    bool needRealloc = needReconfig || (formatConvertFrame_.get()->nb_samples < outSamples) ||
                       !formatConvertFrame_.get()->data[0];

    if (needRealloc) {
        // 设置采样数并分配缓冲区
        formatConvertFrame_.setNbSamples(outSamples);
        int ret = av_frame_get_buffer(formatConvertFrame_.get(), 0);
        if (ret < 0) {
            av_frame_unref(formatConvertFrame_.get());
            return false;
        }
    }

    // 执行格式转换
    int ret = swr_convert(formatConvertCtx_, formatConvertFrame_.get()->data, outSamples,
                          (const uint8_t **)avFrame->data, avFrame->nb_samples);
    if (ret < 0) {
        return false;
    }

    // 设置输出帧的其他属性
    formatConvertFrame_.setNbSamples(ret);
    formatConvertFrame_.setAvPts(frame.avPts());
    formatConvertFrame_.setPktDts(frame.pktDts());

    // 将转换后的数据替换原帧数据
    av_frame_unref(avFrame);
    av_frame_move_ref(avFrame, formatConvertFrame_.get());

    return true;
}

void AudioDecoder::cleanupResampleResources()
{
    if (swrCtx_) {
        swr_free(&swrCtx_);
        swrCtx_ = nullptr;
    }
    needResample_ = false;
}

void AudioDecoder::cleanupFormatConvertResources()
{
    if (formatConvertCtx_) {
        swr_free(&formatConvertCtx_);
        formatConvertCtx_ = nullptr;
    }
    // 重置缓存参数
    lastSrcFormat_ = AV_SAMPLE_FMT_NONE;
    lastDstFormat_ = AV_SAMPLE_FMT_NONE;
    lastConvertSampleRate_ = 0;
    lastConvertChannels_ = 0;
    lastConvertChannelLayout_ = 0;
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END
