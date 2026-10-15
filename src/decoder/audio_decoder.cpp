#include "audio_decoder.h"

#include "demuxer/demuxer.h"
#include "event_system/event_dispatcher.h"
#include "logger/logger.h"
#include "utils/common_utils.h"

namespace {
constexpr char kAudioDecoderName[] = "Audio Decoder";
}

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

AudioDecoder::AudioDecoder(std::shared_ptr<Demuxer> demuxer,
                           std::shared_ptr<EventDispatcher> eventDispatcher,
                           std::shared_ptr<SeekCoordinator> seekCoordinator,
                           std::shared_ptr<FrameQueue> frameQueue)
    : DecoderBase(demuxer, eventDispatcher, seekCoordinator, std::move(frameQueue))
{
}

AudioDecoder::~AudioDecoder()
{
    close();
}

AVMediaType AudioDecoder::type() const
{
    return AVMEDIA_TYPE_AUDIO;
}

std::optional<DecoderInfo> AudioDecoder::buildDecoderInfo() const
{
    if (!isOpened_ || !codecCtx_)
        return std::nullopt;

    DecoderInfo info;
    info.codecName = codecCtx_->codec->name;
    info.mediaType = MediaType::kAudio;
    return info;
}

const char *const AudioDecoder::decoderName() const
{
    return kAudioDecoderName;
}

void AudioDecoder::decodeLoop()
{
    // 解码帧
    Frame frame;
    frame.ensureAllocated();
    if (!frame.isValid()) {
        LOG_ERROR("Audio Decoder decodeLoop error: Failed to allocate frame!");
        handleDecodeError(AVERROR(ENOMEM));
    }

    auto packetQueue = demuxer_->packetQueue(type());
    if (!packetQueue) {
        LOG_ERROR(
            "Audio Decoder decodeLoop error: Can not find packet queue from "
            "demuxer!");
        handleDecodeError(AVERROR_DEMUXER_NOT_FOUND);
        return;
    }

    // 当前packet queue的序号
    auto serial = packetQueue->serial();
    // 上一次上报给seek的序号
    auto lastReportedSerial = serial;

    bool readFirstFrame = false;
    bool occuredError = false;
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
            continue;
        }

        // 检查序列号变化
        if (checkAndUpdateSerial(serial, packetQueue.get())) {
            // 序列号发生变化时，重置下列数据
            // 清空音频队列
            frameQueue_->clear();

            // 清空解码器缓冲区
            avcodec_flush_buffers(codecCtx_);
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
            frame.unref();
            ret = avcodec_receive_frame(codecCtx_, frame.get());
            if (ret < 0) {
                if (ret == AVERROR(EAGAIN)) {
                    // 需要更多输入数据，跳出内层循环继续读取packet
                    break;
                } else {
                    // 其他错误（如EOF），处理错误
                    if (handleDecodeError(ret)) {
                        occuredError = true;
                    }
                    break;
                }
            }
            const auto currentTime = std::chrono::system_clock::now();

            // 成功接收到一帧，直接输出原始解码数据，音频转换由应用层 AudioConverter 负责。
            Frame outputFrame = frame;

            // 计算帧持续时间(单位 s)
            const double duration =
                static_cast<double>(frame.get()->nb_samples) / codecCtx_->sample_rate;
            // 计算PTS（单位s）
            const double pts = calculatePts(outputFrame);

            // 处理 seek 抛帧逻辑 (事务驱动)
            if (shouldDiscardBySeek(pts, serial)) {
                continue;
            }
            // 到达目标位置，上报进度以尝试闭环 Seek 事务
            if (seekCoordinator_ && serial != lastReportedSerial) {
                seekCoordinator_->reportReachedTarget(true, serial);
                lastReportedSerial = serial;
            }

            // 如果是第一帧，发出事件
            if (!readFirstFrame) {
                readFirstFrame = true;
                handleFirstFrame();
            }

            // 处理恢复
            if (occuredError) {
                occuredError = false;
                handleDecodeRecovery();
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

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END
