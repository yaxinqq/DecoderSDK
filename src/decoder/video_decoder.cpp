#include "video_decoder.h"

#include <chrono>
#include <thread>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

#include "demuxer/demuxer.h"
#include "event_system/event_dispatcher.h"
#include "logger/logger.h"
#include "stream_sync/stream_sync_manager.h"
#include "utils/common_utils.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace {
const std::string kVideoDecoderName = "Video Decoder";

// 硬解出错，降级到软解的容忍时间
constexpr int kDefaultFallbackToleranceTime = 2000; // 单位：毫秒

/**
 * @brief 获取合法的H264 profile列表
 * @return 合法的H264 profile列表
 */
const std::vector<uint8_t> &getValidH264Profiles()
{
    static const std::vector<uint8_t> validProfiles = {
        66,  // Baseline
        77,  // Main
        88,  // Extended
        100, // High
        110, // High 10
        118, // Multiview High
        122, // High 422
        128, // Stereo High
        144, // High 444
        244, // High 444 Predictive
        44   // CAVLC 444
    };
    return validProfiles;
}

/**
 * @brief 在AnnexB格式数据中查找SPS NAL单元
 * @param data 数据指针
 * @param size 数据大小
 * @param sps_profile_ptr 输出参数：SPS profile_idc字段的指针
 * @param sps_constraint_ptr 输出参数：SPS constraint字段的指针
 * @return true 如果找到SPS，false 如果未找到
 */
bool findSPSInAnnexB(const uint8_t *data, int size, uint8_t **sps_profile_ptr,
                     uint8_t **sps_constraint_ptr)
{
    if (!data || size < 8) {
        return false;
    }

    const uint8_t *end = data + size;
    const uint8_t *current = data;

    while (current < end - 4) {
        // 查找起始码 (0x000001 或 0x00000001)
        if (current[0] == 0x00 && current[1] == 0x00 &&
            ((current[2] == 0x01) || (current[2] == 0x00 && current[3] == 0x01))) {
            const int offset = (current[2] == 0x01) ? 3 : 4;
            const uint8_t *nal_start = current + offset;

            if (nal_start >= end)
                break;

            // 检查NAL类型是否为SPS (type = 7)
            uint8_t nal_type = nal_start[0] & 0x1F;
            if (nal_type == 7 && nal_start + 2 < end) {
                // 找到SPS
                *sps_profile_ptr = const_cast<uint8_t *>(&nal_start[1]);
                *sps_constraint_ptr = const_cast<uint8_t *>(&nal_start[2]);
                return true;
            }

            // 移动到下一个可能的起始码位置
            current = nal_start;
        } else {
            ++current;
        }
    }

    return false;
}

/**
 * @brief 检查并修正H264 profile
 * @param profile_idc 当前profile值
 * @param sps_profile_ptr profile_idc字段的指针
 * @param sps_constraint_ptr constraint字段的指针
 * @param format_name 格式名称（用于日志）
 * @param showLog 是否输出日志
 * @return true 如果进行了修正，false 如果无需修正
 */
bool fixH264ProfileCommon(uint8_t profile_idc, uint8_t *sps_profile_ptr,
                          uint8_t *sps_constraint_ptr, const char *format_name,
                          bool showLog = false)
{
    if (!sps_profile_ptr || !sps_constraint_ptr) {
        return false;
    }

    const auto &validProfiles = getValidH264Profiles();

    // 检查当前profile是否在合法列表中
    const bool isValidProfile =
        std::find(validProfiles.begin(), validProfiles.end(), profile_idc) != validProfiles.end();

    if (isValidProfile) {
        return false; // profile合法，无需修正
    }

    // profile不合法，强制改为baseline profile (66)
    *sps_profile_ptr = 66; // FF_PROFILE_H264_BASELINE

    // 同时更新profile_compatibility字段为baseline兼容
    // baseline profile的constraint_set0_flag + constraint_set1_flag应该设置为1
    *sps_constraint_ptr = 0xc0;

    // 记录日志
    if (showLog) {
        LOG_WARN("H264 profile {} is invalid, forced to baseline profile (66) in {} format",
                 profile_idc, format_name);
    }

    return true;
}

/**
 * @brief 检查并修正AnnexB格式AVPacket中SPS的profile
 * @param pkt AVPacket指针
 * @return true 如果进行了修正，false 如果无需修正或修正失败
 */
bool fixAnnexBSPSProfileInPacket(AVPacket *pkt)
{
    if (!pkt || !pkt->data || pkt->size < 8) {
        return false;
    }

    uint8_t *sps_profile_ptr = nullptr;
    uint8_t *sps_constraint_ptr = nullptr;

    // 查找SPS NAL单元
    if (!findSPSInAnnexB(pkt->data, pkt->size, &sps_profile_ptr, &sps_constraint_ptr)) {
        return false;
    }

    const uint8_t profile_idc = *sps_profile_ptr;
    return fixH264ProfileCommon(profile_idc, sps_profile_ptr, sps_constraint_ptr, "AnnexB packet");
}

/**
 * @brief 检查H264 SPS中的profile是否合法，如果不合法则强制改为baseline profile
 * @param codecCtx 解码器上下文
 * @return true 如果进行了修正，false 如果无需修正或修正失败
 */
bool fixH264ProfileIfNeeded(AVCodecContext *codecCtx)
{
    if (!codecCtx || codecCtx->codec_id != AV_CODEC_ID_H264 || !codecCtx->extradata ||
        codecCtx->extradata_size < 8) {
        return false;
    }

    const uint8_t *data = codecCtx->extradata;
    const int size = codecCtx->extradata_size;
    const bool isAVCC = (data[0] == 0x01);

    uint8_t profile_idc = 0;
    uint8_t *sps_profile_ptr = nullptr;
    uint8_t *sps_constraint_ptr = nullptr;

    if (isAVCC) {
        // AVCC格式处理
        if (size < 8)
            return false;
        profile_idc = data[1];
        sps_profile_ptr = const_cast<uint8_t *>(&data[1]);
        sps_constraint_ptr = const_cast<uint8_t *>(&data[2]);
    } else {
        // AnnexB格式处理 - 查找SPS NAL单元
        if (!findSPSInAnnexB(data, size, &sps_profile_ptr, &sps_constraint_ptr)) {
            return false; // 未找到SPS
        }
        profile_idc = *sps_profile_ptr;
    }

    return fixH264ProfileCommon(profile_idc, sps_profile_ptr, sps_constraint_ptr,
                                isAVCC ? "AVCC" : "AnnexB", true);
}

bool isAnnexBFormat(const uint8_t *data, size_t size)
{
    if (size >= 4) {
        return (data[0] == 0 && data[1] == 0 && ((data[2] == 0 && data[3] == 1) || data[2] == 1));
    }
    return false;
}
} // namespace

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

    // 新增：配置硬件解码退化选项
    enableHardwareFallback_ = config.enableHardwareFallback;
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
    bool transToAVCC = false;
    std::optional<std::chrono::high_resolution_clock::time_point> errorStartTime;

    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;

    resetStatistics();
    while (isRunning_.load()) {
        // 如果在等待预缓冲，则暂停解码
        if (waitingForPreBuffer_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 处理暂停状态
        if (isPaused_.load()) {
            std::unique_lock<std::mutex> lock(pauseMutex_);
            pauseCv_.wait_for(lock, std::chrono::milliseconds(10),
                              [this] { return !isPaused_.load() || !isRunning_.load(); });
            if (!isRunning_.load()) {
                break;
            }
            if (isPaused_.load())
                continue;

            // 重置最后帧时间
            lastFrameTime_ = std::nullopt;
            // 重置第一帧读取状态
            readFirstFrame = false;
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

            std::lock_guard<std::mutex> lock(configMutex_);
            demuxerSeeking_ = false;
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

        if (codecCtx_->codec_id == AV_CODEC_ID_H264 && hwAccel_ &&
            (hwAccel_->getType() == HWAccelType::kD3d11va ||
             hwAccel_->getType() == HWAccelType::kDxva2) &&
            needFixSPSProfile_) {
            if (isAnnexBFormat(packet.get()->data, packet.get()->size)) {
                // 检查是否是IDR帧（关键帧）
                bool isIDRFrame = (packet.get()->flags & AV_PKT_FLAG_KEY) != 0;

                // 如果是关键帧，检查并修正SPS profile
                if (isIDRFrame) {
                    fixAnnexBSPSProfileInPacket(packet.get());
                }
            }
        }

        // 发送包到解码器
        int ret = avcodec_send_packet(codecCtx_, packet.get());
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            // 判断是否需要退化到软解
            if (readFirstFrame || !shouldFallbackToSoftware(ret))
                continue;

            // 记录出错时间
            const auto currentTime = std::chrono::high_resolution_clock::now();
            if (!errorStartTime.has_value()) {
                errorStartTime = currentTime; // 记录第一次错误时间
            }

            // 判断是否超过容忍时间
            if (std::chrono::duration_cast<std::chrono::milliseconds>(currentTime -
                                                                      errorStartTime.value())
                    .count() < kDefaultFallbackToleranceTime) {
                continue; // 未超过容忍时间，继续等待
            }

            // 如果超过容忍时间，尝试软解
            if (reinitializeWithSoftwareDecoder()) {
                LOG_INFO("Video Decoder: Fallback to software decoding.");
                hasKeyFrame = false;    // 重置关键帧标志
                errorStartTime.reset(); // 重置错误计时
            } else {
                LOG_ERROR("Video Decoder: Failed to reinitialize with software decoder.");
                break; // 退出解码循环
            }
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

        // 如果当前小于seekPos，丢弃帧
        {
            std::lock_guard<std::mutex> l(configMutex_);
            // 如果当前正在seeking，直接跳过
            if (demuxerSeeking_)
                continue;
            if (utils::greater(seekPos_, 0)) {
                if (!utils::greaterAndEqual(pts, seekPos_)) {
                    frame.unref();
                    continue;
                }
                seekPos_ = -1.0;
            }
        }

        // 如果是第一帧，发出事件
        if (!readFirstFrame) {
            readFirstFrame = true;
            errorStartTime.reset(); // 重置错误计时
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
        outFrame->setMediaType(AVMEDIA_TYPE_VIDEO);

        // 如果启用了帧率控制，则根据帧率控制推送速度
        if (isFrameRateControlEnabled()) {
            // 计算基本延迟
            const double baseDelay =
                calculateFrameDisplayTime(pts, duration * 1000.0, lastFrameTime_);

            // 使用同步控制器计算实际延迟
            const double syncDelay =
                syncController_->computeVideoDelay(pts, duration, baseDelay, speed());

            // 检查是否需要丢弃此帧
            if (syncDelay < 0) {
                // 丢弃此帧，不提交到队列
                frame.unref();
                continue;
            }

            // 使用同步后的延迟
            if (utils::greater(syncDelay, 0.0)) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(static_cast<int64_t>(syncDelay)));
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
    needFixSPSProfile_ = false; // 重置SPS修正标志

    // 创建硬件加速器（默认尝试自动选择最佳硬件加速方式）
    hwAccel_ = HardwareAccelFactory::getInstance().createHardwareAccel(hwAccelType_, deviceIndex_,
                                                                       hwContextCallbeck_);
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

    // 如果创建的是D3D11、DXVA2类型的硬解码器，且是H264编码，则修复异常的SPS profile
    if ((hwAccel_->getType() == HWAccelType::kD3d11va ||
         hwAccel_->getType() == HWAccelType::kDxva2) &&
        codecCtx_->codec_id == AV_CODEC_ID_H264 && codecCtx_->extradata &&
        codecCtx_->extradata_size > 0) {
        needFixSPSProfile_ = fixH264ProfileIfNeeded(codecCtx_);
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

bool VideoDecoder::shouldFallbackToSoftware(int errorCode) const
{
    // 检查退化条件：
    // 1. 启用了硬件解码退化功能
    // 2. 当前使用硬件解码（有硬件设备上下文）
    // 3. 错误码是 AVERROR_INVALIDDATA
    // 4. 还未解码出过视频帧
    // 5. 硬件解码还未失败过（避免重复尝试）
    return enableHardwareFallback_ && codecCtx_ && codecCtx_->hw_device_ctx &&
           errorCode == AVERROR_INVALIDDATA;
}

bool VideoDecoder::reinitializeWithSoftwareDecoder()
{
    LOG_INFO("Attempting to reinitialize decoder with software decoding");

    // 关闭当前解码器
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
    }

    // 重置硬件加速器
    hwAccel_.reset();
    needFixSPSProfile_ = false; // 重置SPS修正标志

    // 重新打开解码器（不使用硬件加速）
    auto *const formatContext = demuxer_->formatContext();
    if (!formatContext) {
        LOG_ERROR("Format context is null during software fallback");
        return false;
    }

    stream_ = formatContext->streams[streamIndex_];
    if (!stream_) {
        LOG_ERROR("Stream is null during software fallback");
        return false;
    }

    // 查找解码器（强制使用软件解码器）
    const AVCodec *codec = avcodec_find_decoder(stream_->codecpar->codec_id);
    if (!codec) {
        LOG_ERROR("Software decoder not found for codec {}",
                  static_cast<int>(stream_->codecpar->codec_id));
        return false;
    }

    // 分配解码器上下文
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        LOG_ERROR("Failed to allocate software decoder context");
        return false;
    }

    // 复制流参数到解码器上下文
    int ret = avcodec_parameters_to_context(codecCtx_, stream_->codecpar);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to copy stream parameters to software decoder context: {}", errBuf);
        avcodec_free_context(&codecCtx_);
        return false;
    }

    // 打开软件解码器（不设置硬件加速）
    ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to open software decoder: {}", errBuf);
        avcodec_free_context(&codecCtx_);
        return false;
    }

    LOG_INFO("Successfully switched to software decoding for codec: {}", codec->name);

    // 刷新解码器缓冲区
    avcodec_flush_buffers(codecCtx_);

    return true;
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END