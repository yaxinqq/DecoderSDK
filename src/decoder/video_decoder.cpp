#include "video_decoder.h"

#include <chrono>
#include <thread>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/rational.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

#include "demuxer/demuxer.h"
#include "event_system/event_dispatcher.h"
#include "logger/logger.h"
#include "utils/common_utils.h"

#ifdef VAAPI_AVAILABLE
#include "vaapi/vaapi_utils.h"
#endif

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace {
constexpr char kVideoDecoderName[] = "Video Decoder";

// 硬解出错，降级到软解的容忍时间
constexpr int kDefaultFallbackToleranceTime = 2000; // 单位：毫秒
// send packet持续出错最大次数
constexpr int kSendPacketMaxErrorCount = 25;

bool frameIsKey(const Frame &frame)
{
    if (!frame.isValid())
        return false;

#if LIBAVUTIL_VERSION_MAJOR >= 58
    if (frame.get()->flags & AV_FRAME_FLAG_KEY) {
#else
    if (frame.get()->key_frame == 1) {
#endif
        return true;
    }

    return false;
}

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

/**
 * @brief 检查数据是否为Annex-B格式
 * @param data 输入数据
 * @param size 数据大小
 * @return true 如果是Annex-B格式，false 否则
 */
bool isAnnexBFormat(const uint8_t *data, size_t size)
{
    if (size >= 4) {
        return (data[0] == 0 && data[1] == 0 && ((data[2] == 0 && data[3] == 1) || data[2] == 1));
    }
    return false;
}

/**
 * @brief 从Annex-B流中分割NALU
 * @param data 输入数据
 * @param size 数据大小
 * @return NALU列表
 */
size_t findAnnexBStartCode(const uint8_t *data, size_t size, size_t from, size_t &startCodeSize)
{
    // 从指定偏移开始扫描起始码
    for (size_t p = from; p + 3 <= size; ++p) {
        // 优先匹配 4 字节起始码 00 00 00 01
        if (p + 4 <= size && data[p] == 0x00 && data[p + 1] == 0x00 && data[p + 2] == 0x00 &&
            data[p + 3] == 0x01) {
            // 返回起始码长度给调用方
            startCodeSize = 4;
            // 返回起始码位置
            return p;
        }
        // 匹配 3 字节起始码 00 00 01
        if (data[p] == 0x00 && data[p + 1] == 0x00 && data[p + 2] == 0x01) {
            // 返回起始码长度给调用方
            startCodeSize = 3;
            // 返回起始码位置
            return p;
        }
    }

    // 未找到起始码时长度置 0
    startCodeSize = 0;
    // 返回 size 作为“未找到”的哨兵值
    return size;
}

/**
 * @brief 去除emulation prevention bytes
 * @param data 输入数据
 * @param size 数据大小
 * @return 处理后的数据
 */
std::vector<uint8_t> removeEPB(const uint8_t *data, size_t size)
{
    // 输出 RBSP 数据
    std::vector<uint8_t> out;
    // 预留容量，减少扩容次数
    out.reserve(size);
    // 顺序扫描输入字节流
    for (size_t i = 0; i < size; ++i) {
        // 检测 emulation prevention 三字节序列 00 00 03
        if (i + 2 < size && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x03) {
            // 保留前两个 0x00
            out.push_back(0x00);
            out.push_back(0x00);
            // 跳过 0x03
            i += 2;
            continue;
        }
        // 非 EPB 序列则原样拷贝
        out.push_back(data[i]);
    }
    // 返回去除 EPB 后的数据
    return out;
}

/**
 * @brief 解析单个NALU中的SEI数据
 * @param nal NALU数据
 * @param nalSize NALU大小
 * @param codecIsHevc 是否为HEVC编码
 * @return SEI数据列表
 *
 * 根据H.264/H.265标准实现：
 * - payloadType值为5表示user_data_unregistered
 * - payloadType/payloadSize为可变长编码，使用0xFF累加机制
 * - UUID占16字节，剩余部分为payload
 */
void appendSeiFromNal(const uint8_t *nal, size_t nalSize, bool codecIsHevc,
                      std::vector<UserSEIData> &results)
{
    // 空 NAL 直接返回
    if (nalSize == 0)
        return;

    // NAL 类型，先置无效值
    int nalUnitType = -1;
    // 默认按 H.264 头部长度 1 字节
    size_t headerSize = 1;

    // H.264 分支
    if (!codecIsHevc) {
        // H.264 NAL type 位于低 5 位
        nalUnitType = nal[0] & 0x1F;
        // 仅处理 SEI(type=6)
        if (nalUnitType != 6)
            return;
    } else {
        // HEVC 头部至少 2 字节
        if (nalSize < 2)
            return;
        // HEVC NAL type 为 6 位
        nalUnitType = (nal[0] >> 1) & 0x3F;
        // 仅处理 prefix SEI(type=39) 或 suffix SEI(type=40)
        if (nalUnitType != 39 && nalUnitType != 40)
            return;
        // HEVC NAL 头部长度为 2 字节
        headerSize = 2;
    }

    // 去除 NAL 负载中的 EPB（Emulation Prevention Bytes: 00 00 03），得到 RBSP
    auto rbsp = removeEPB(nal + headerSize, nalSize - headerSize);
    // RBSP 为空则无可解析内容
    if (rbsp.empty())
        return;

    // RBSP 当前解析偏移
    size_t off = 0;

    // 解析所有 SEI message，直到缓冲区用尽
    while (off < rbsp.size()) {
        // ========== 解析可变长 payloadType ==========
        // 按照规范：连续的0xFF字节对应累加255，直到非0xFF字节
        unsigned payloadType = 0;
        while (off < rbsp.size()) {
            uint8_t b = rbsp[off++];
            payloadType += b;
            if (b != 0xFF)
                break;
        }
        
        // 如果payloadType解析后已消耗到末尾，无法继续读取payloadSize，退出
        if (off >= rbsp.size())
            break;

        // ========== 解析可变长 payloadSize ==========
        // 同样的0xFF累加机制
        unsigned payloadSize = 0;
        while (off < rbsp.size()) {
            uint8_t b = rbsp[off++];
            payloadSize += b;
            if (b != 0xFF)
                break;
        }

        // ========== 边界保护 ==========
        // 检查payload数据是否越界
        if (off + payloadSize > rbsp.size())
            break;

        // ========== 提取 user_data_unregistered SEI (payloadType==5) ==========
        // 根据规范，此类型SEI的payload结构为：16字节UUID + 内容数据
        if (payloadType == 5 && payloadSize >= 16) {
            UserSEIData sei;
            // 前 16 字节为 UUID
            memcpy(sei.uuid.data(), rbsp.data() + off, 16);
            // 剩余部分为用户 payload
            const size_t payloadLen = payloadSize - 16;
            sei.payload.resize(payloadLen);
            if (payloadLen > 0) {
                memcpy(sei.payload.data(), rbsp.data() + off + 16, payloadLen);
            }
            // 追加到输出列表
            results.emplace_back(std::move(sei));
        }

        // ========== 跳到下一个 SEI message ==========
        off += payloadSize;
    }
}

/**
 * @brief 解析数据包中的SEI信息
 * @param packet 数据包
 * @param codecIsHevc 编码格式是否为HEVC（H.265）
 * @return SEI数据列表
 *
 * 实现细节：
 * 1. 支持Annex-B和AVCC两种码流格式的自动识别
 * 2. 按照H.264/H.265标准解析NAL单元中的SEI消息
 * 3. 正确处理payloadType和payloadSize的可变长编码（0xFF累加机制）
 * 4. 移除Emulation Prevention Bytes（EPB）恢复RBSP数据
 */
std::vector<UserSEIData> parseSEIFromPacket(const Packet &packet, bool codecIsHevc)
{
    // 包对象、数据指针或大小非法时直接返回空结果
    if (!packet.get() || !packet.get()->data || packet.get()->size <= 0) {
        return {};
    }

    // 原始码流数据指针
    const uint8_t *data = packet.get()->data;
    // 原始码流总大小
    size_t size = packet.get()->size;

    // 判断是否为 Annex-B 格式（否则为AVCC格式）
    bool isAnnexB = isAnnexBFormat(data, size);

    // 最终 SEI 汇总结果
    std::vector<UserSEIData> results;

    if (isAnnexB) {
        // ========== Annex-B 路径：按起始码逐段提取 NAL ==========
        // 记录当前起始码长度（3 或 4）
        size_t startCodeSize = 0;
        // 查找第一个起始码
        size_t start = findAnnexBStartCode(data, size, 0, startCodeSize);

        // 若未找到起始码，按单个 NAL 直接解析
        if (start == size) {
            appendSeiFromNal(data, size, codecIsHevc, results);
            return results;
        }

        // 如果第一个起始码不在位置0
        // 说明第一个NAL前面没有起始码前缀，需要特别处理
        if (start > 0) {
            // 处理第一个NAL（从0到第一个起始码）
            appendSeiFromNal(data, start, codecIsHevc, results);
        }

        // 逐个 NAL 扫描并解析 Annex-B 格式的数据
        while (start < size) {
            // 当前 NAL 数据起点（跳过起始码）
            const size_t nalStart = start + startCodeSize;
            // 边界保护
            if (nalStart >= size) {
                break;
            }

            // 查找下一个起始码
            size_t nextStartCodeSize = 0;
            const size_t next = findAnnexBStartCode(data, size, nalStart, nextStartCodeSize);
            // 当前 NAL 终点为下一个起始码位置或流尾
            const size_t nalEnd = (next == size) ? size : next;
            // NAL 区间合法才解析
            if (nalStart < nalEnd) {
                appendSeiFromNal(data + nalStart, nalEnd - nalStart, codecIsHevc, results);
            }
            // 没有下一个起始码，扫描结束
            if (next == size) {
                break;
            }

            // 滚动到下一个 NAL
            start = next;
            startCodeSize = nextStartCodeSize;
        }
    } else {
        // ========== AVCC 路径：按 4 字节长度前缀逐个读取 NAL ==========
        size_t pos = 0;
        while (pos + 4 <= size) {
            // 读取大端 NAL 长度前缀
            const uint32_t nalSize = (uint32_t(data[pos]) << 24) | (uint32_t(data[pos + 1]) << 16) |
                                     (uint32_t(data[pos + 2]) << 8) | uint32_t(data[pos + 3]);
            // 跳过长度前缀
            pos += 4;
            // 越界保护
            if (pos + nalSize > size) {
                break;
            }
            // 仅解析非空 NAL
            if (nalSize > 0) {
                appendSeiFromNal(data + pos, nalSize, codecIsHevc, results);
            }
            // 移动到下一个长度前缀
            pos += nalSize;
        }
    }

    // 返回整包 SEI 结果
    return results;
}

#ifdef VAAPI_AVAILABLE
// VaapiSurfaceEGLExportData对应的AVBufferRef的清理回调
static void vaapiSurfaceEGLExportDataFree(void *opaque, uint8_t *data)
{
    if (!data)
        return;

    auto *externalData = reinterpret_cast<VaapiSurfaceEGLExportData *>(data);

    // 关闭所有有效 fd
    for (uint32_t i = 0; i < externalData->numObjects; ++i) {
        if (externalData->objects[i].fd >= 0) {
            close(externalData->objects[i].fd);
            externalData->objects[i].fd = -1;
        }
    }

    // 清理内存
    av_free(externalData);
}

// 创建VaapiSurfaceEGLExportData对应的AVBufferRef
AVBufferRef *createVaapiSurfaceEGLExportDataBuffer(const VADRMPRIMESurfaceDescriptor &desc)
{
    // 分配 VaapiSurfaceEGLExportData
    VaapiSurfaceEGLExportData *data = reinterpret_cast<VaapiSurfaceEGLExportData *>(
        av_mallocz(sizeof(VaapiSurfaceEGLExportData)));
    if (!data)
        return nullptr;

    // 复制基本信息
    data->fourcc = desc.fourcc;
    data->width = desc.width;
    data->height = desc.height;

    // 复制对象信息
    data->numObjects = desc.num_objects;
    for (uint32_t i = 0; i < desc.num_objects; ++i) {
        data->objects[i].fd = desc.objects[i].fd;
        data->objects[i].size = desc.objects[i].size;
        data->objects[i].drmFormatModifier = desc.objects[i].drm_format_modifier;
    }

    // 复制 layer 信息
    data->numLayers = desc.num_layers;
    for (uint32_t l = 0; l < desc.num_layers; ++l) {
        const auto &srcLayer = desc.layers[l];
        auto &dstLayer = data->layers[l];

        dstLayer.drmFormat = srcLayer.drm_format;
        dstLayer.numPlanes = srcLayer.num_planes;

        for (uint32_t p = 0; p < srcLayer.num_planes; ++p) {
            dstLayer.objectIndex[p] = srcLayer.object_index[p];
            dstLayer.offset[p] = srcLayer.offset[p];
            dstLayer.pitch[p] = srcLayer.pitch[p];
        }
    }

    // 创建 AVBufferRef
    AVBufferRef *buf =
        av_buffer_create(reinterpret_cast<uint8_t *>(data), sizeof(VaapiSurfaceEGLExportData),
                         vaapiSurfaceEGLExportDataFree, nullptr, 0);

    if (!buf) {
        // 创建失败，释放内存
        av_free(data);
        return nullptr;
    }

    return buf;
}

void clearSurfaceCache(std::unordered_map<VASurfaceID, AVBufferRef *> &surfaceCache)
{
    // 清理surface缓存
    for (auto &pair : surfaceCache) {
        if (pair.second) {
            av_buffer_unref(&pair.second);
        }
    }
    surfaceCache.clear();
}
#endif

} // namespace

VideoDecoder::VideoDecoder(std::shared_ptr<Demuxer> demuxer,
                           std::shared_ptr<EventDispatcher> eventDispatcher,
                           std::shared_ptr<SeekCoordinator> seekCoordinator)
    : DecoderBase(demuxer, eventDispatcher, seekCoordinator)
{
    init({});
}

VideoDecoder::~VideoDecoder()
{
    close();

    // 确保所有帧都被释放
    memoryFrame_.release();
    swsFrame_.release();

    // 释放软件缩放上下文
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }

    // 重置硬件加速器
    hwAccel_.reset();
}

void VideoDecoder::init(const DecoderConfig &config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    hwAccelType_ = config.hwAccelType;
    backendHwAccelType_ = config.backendHwAccelType;
    deviceIndex_ = config.hwDeviceIndex;
    softPixelFormat_ = utils::imageFormat2AVPixelFormat(config.swVideoOutFormat);
    requireFrameInMemory_ = config.requireFrameInSystemMemory;
    createHWContextCallback_ = config.createHwContextCallback;
    freeHWContextCallback_ = config.freeHwContextCallback;
    enableHardwareFallback_ = config.enableHardwareFallback;
    enableParseUserSEIData_ = config.enableParseUserSEIData;
}

AVMediaType VideoDecoder::type() const
{
    return AVMEDIA_TYPE_VIDEO;
}

void VideoDecoder::requireFrameInSystemMemory(bool required)
{
    std::lock_guard<std::mutex> lock(mutex_);
    requireFrameInMemory_ = required;
}

double VideoDecoder::getFrameRate() const
{
    AVRational frameRate = av_guess_frame_rate(demuxer_->formatContext(), stream_, NULL);
    return av_q2d(frameRate);
}

void VideoDecoder::calculateBitsPerPixel()
{
    if (bppCalculated_ || !codecCtx_ || codecCtx_->width <= 0 || codecCtx_->height <= 0)
        return;

    // 通过 FFmpeg 获取该像素格式在指定宽高下的缓冲区大小（字节），再换算为每像素比特数
    const int bufferSize =
        av_image_get_buffer_size(codecCtx_->sw_pix_fmt, codecCtx_->width, codecCtx_->height, 1);
    if (bufferSize > 0) {
        bitsPerPixel_ = (bufferSize * 8) / (codecCtx_->width * codecCtx_->height);
    }
    bppCalculated_ = true;
}

std::optional<DecoderInfo> VideoDecoder::decoderInfo() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!isOpened_ || !codecCtx_)
        return std::nullopt;

    DecoderInfo info;
    info.codecName = codecCtx_->codec->name;
    info.mediaType = MediaType::kVideo;
    info.hwAccelType = hwAccel_ ? hwAccel_->getType() : HWAccelType::kNone;
    info.bitsPerPixel = bitsPerPixel_;
    return info;
}

const char *const VideoDecoder::decoderName() const
{
    return kVideoDecoderName;
}

void VideoDecoder::decodeLoop()
{
    // 解码帧
    Frame frame;
    frame.ensureAllocated();
    if (!frame.isValid()) {
        LOG_ERROR("Video Decoder decodeLoop error: Failed to allocate frame!");
        handleDecodeError(AVERROR(ENOMEM));
    }

    auto packetQueue = demuxer_->packetQueue(type());
    if (!packetQueue) {
        LOG_ERROR(
            "Video Decoder decodeLoop error: Can not find packet queue from "
            "demuxer!");
        handleDecodeError(AVERROR_DEMUXER_NOT_FOUND);
        return;
    }

#ifdef VAAPI_AVAILABLE
    // Surface缓存池
    std::unordered_map<VASurfaceID, AVBufferRef *> surfaceCache;

    // vadisplay
    auto *vaDisplay = hwAccel_ ? hwAccel_->getVADisplay() : nullptr;
#endif

    // 当前packet queue的序号
    auto serial = packetQueue->serial();
    // 上一次上报给seek的序号
    auto lastReportedSerial = serial;

    bool hasKeyFrame = false;
    bool readFirstFrame = false;
    bool occuredError = false;
    bool transToAVCC = false;
    std::optional<std::chrono::system_clock::time_point> errorStartTime;
    int sendPacketErrorCount = 0;

    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;

    resetStatistics();
    const auto isRealTime = demuxer_->isRealTime();

    // 如果是实时流，此时应该清空包队列
    if (isRealTime) {
        packetQueue->flush();
    }

    // 解码器id
    auto codecId = codecCtx_->codec_id;
    // 目前推测到的帧率
    const auto avgDuration = 1 / getFrameRate();
    // 是否需要强行同步外部时钟
    bool forceSyncExternalClock = false;
    while (!requestInterruption_.load()) {
        // 如果在等待预缓冲，则暂停解码
        if (waitingForPreBuffer_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 处理暂停状态
        if (isPaused_.load()) {
            std::unique_lock<std::mutex> lock(pauseMutex_);
            pauseCv_.wait_for(lock, std::chrono::milliseconds(10),
                              [this] { return !isPaused_.load() || requestInterruption_.load(); });
            if (requestInterruption_.load()) {
                break;
            }
            if (isPaused_.load())
                continue;

            // 重置第一帧读取状态
            readFirstFrame = false;
            continue;
        }

        // 检查序列号变化
        if (checkAndUpdateSerial(serial, packetQueue.get())) {
            // 序列号发生变化时，重置下列数据
            // 重新等待关键帧
            hasKeyFrame = false;
            // 清空帧队列
            frameQueue_->clear();

            // 清空解码器缓冲区
            avcodec_flush_buffers(codecCtx_);
        }

        // 从包队列中获取一个包
        Packet packet;
        bool gotPacket = packetQueue->pop(packet, 1);
        if (!gotPacket) {
            // 没有包可用，可能是队列为空或已中止
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 检查序列号
        if (packet.serial() != serial)
            continue;

        // 等待关键帧
        const bool isKeyFrame = (packet.get()->flags & AV_PKT_FLAG_KEY) != 0;
        if (!hasKeyFrame && !isKeyFrame) {
            continue;
        }
        if (!hasKeyFrame && isKeyFrame) {
            forceSyncExternalClock = true;
            hasKeyFrame = true;
        }

        // 处理SPS profile可能存在的错误
        if (codecId == AV_CODEC_ID_H264 && hwAccel_ &&
            (hwAccel_->getType() == HWAccelType::kD3d11va ||
             hwAccel_->getType() == HWAccelType::kDxva2 ||
             hwAccel_->getType() == HWAccelType::kVaapi) &&
            needFixSPSProfile_) {
            if (isAnnexBFormat(packet.get()->data, packet.get()->size)) {
                // 如果是关键帧，检查并修正SPS profile
                if (isKeyFrame) {
                    fixAnnexBSPSProfileInPacket(packet.get());
                }
            }
        }

        // 发送包到解码器
        int ret = avcodec_send_packet(codecCtx_, packet.get());
        if (ret != 0) {
            // 记录出错的信息
            LOG_WARN("{} send packet error, error code: {}, error string: {}", demuxer_->url(), ret,
                     utils::avErr2Str(ret));

            // 处理解码错误
            handleDecodeError(ret);

            // 是否需要退化到软解
            const auto shouldFallback = !readFirstFrame && shouldFallbackToSoftware(ret);

            // 如果出错的是I帧，且此时不需要退化到软解，则等待下一个I帧恢复正常
            if (isKeyFrame && !shouldFallback) {
                // 处理关键帧错误
                handleKeyFrameError(hasKeyFrame,
                                    "Key frame decode failed, waiting for next key frame");
#ifdef VAAPI_AVAILABLE
                // 清理surface缓存
                clearSurfaceCache(surfaceCache);
#endif
                continue;
            }

            // 如果是EAGAIN或EOF错误，继续等待下一个包
            if (ret == AVERROR(EAGAIN) || ret == AVERROR(EOF)) {
                continue; // 继续
            }

            // 判断是否需要退化到软解
            if (!shouldFallback) {
                // 不需要退化到软解时，需要累计出错次数，当出错次数达到限制时，flush
                if (++sendPacketErrorCount >= kSendPacketMaxErrorCount) {
                    sendPacketErrorCount = 0;

                    // flush
                    avcodec_flush_buffers(codecCtx_);
#ifdef VAAPI_AVAILABLE
                    // 清理surface缓存
                    clearSurfaceCache(surfaceCache);
#endif
                    // 重新等待关键帧
                    hasKeyFrame = false;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // 记录出错时间
            const auto currentTime = std::chrono::system_clock::now();
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
                hasKeyFrame = false;           // 重置关键帧标志
                errorStartTime.reset();        // 重置错误计时
                codecId = codecCtx_->codec_id; // 重置解码器ID
#ifdef VAAPI_AVAILABLE
                // 清理surface缓存
                clearSurfaceCache(surfaceCache);
#endif
            } else {
                LOG_ERROR("Video Decoder: Failed to reinitialize with software decoder.");
                break; // 退出解码循环
            }
            continue;
        }
        sendPacketErrorCount = 0;

        // 循环接收所有可能的解码帧
        while (true) {
            frame.unref();
            ret = avcodec_receive_frame(codecCtx_, frame.get());
            if (ret != 0) {
                if (ret == AVERROR(EAGAIN) || ret == AVERROR(EOF)) {
                    // 需要更多输入数据，跳出内层循环继续读取packet
                    break;
                } else {
                    // 其他错误（如EOF），处理错误
                    if (handleDecodeError(ret)) {
                        occuredError = true;

                        // 如果是I帧，等待下一个I帧过来
                        if (isKeyFrame) {
                            handleKeyFrameError(
                                hasKeyFrame, "Key frame decode failed, waiting for next key frame");
#ifdef VAAPI_AVAILABLE
                            // 清理surface缓存
                            clearSurfaceCache(surfaceCache);
#endif
                        }
                    }
                    break;
                }
            }

            // 出现坏包，跳过
            if (auto *const avFrame = frame.get(); !avFrame ||
                                                   (avFrame->flags & AV_FRAME_FLAG_CORRUPT) != 0 ||
                                                   avFrame->decode_error_flags != 0) {
                // 提示坏包
                LOG_WARN("{} Frame decode corrupt", demuxer_->url());
                handleDecodeError(ret);

                // 如果是I帧，等待下一个I帧过来
                if (isKeyFrame) {
                    handleKeyFrameError(hasKeyFrame,
                                        "Key frame decode failed, waiting for next key frame");
#ifdef VAAPI_AVAILABLE
                    // 清理surface缓存
                    clearSurfaceCache(surfaceCache);
#endif
                }
                break;
            }

            // 成功接收到一帧，进行处理
            // 计算帧持续时间(单位 s)
            const double duration = calculateFrameDuration(frame, avgDuration);
            // 计算PTS（单位s）
            const double pts = calculatePts(frame);

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
                errorStartTime.reset(); // 重置错误计时
                calculateBitsPerPixel();
                handleFirstFrame();
            }

            // 如果恢复，则发出事件
            if (occuredError) {
                occuredError = false;
                handleDecodeRecovery();
            }

            // 处理帧格式转换
            Frame outputFrame = processFrameConversion(frame);
            if (!outputFrame.isValid()) {
                frame.unref();
                continue;
            }

            // 获取一个可写入的帧
            Frame *outFrame = frameQueue_->getWritableFrame();
            if (!outFrame) {
                frame.unref();
                break; // 队列满了，退出
            }

            // 解析数据包中的SEI信息，仅支持H264、H265
            std::vector<UserSEIData> seiDataList;
            if (enableParseUserSEIData_ && isKeyFrame &&
                (codecId == AV_CODEC_ID_H264 || codecId == AV_CODEC_ID_H265)) {
                seiDataList = parseSEIFromPacket(packet, codecId == AV_CODEC_ID_H265);
                LOG_TRACE("Found {} SEI data entries in packet", seiDataList.size());
                for (const auto &seiData : seiDataList) {
                    LOG_TRACE("SEI UUID: {}, payload size: {}, payload: {}", seiData.uuidHex(),
                              seiData.payload.size(), seiData.payloadAsString());
                }
            }

            // 将解码后的帧复制到输出帧
            *outFrame = std::move(outputFrame);
            outFrame->setSerial(serial);
            outFrame->setDurationByFps(duration);
            outFrame->setSecPts(pts);
            outFrame->setMediaType(AVMEDIA_TYPE_VIDEO);
            outFrame->setUserSEIDataList(
                seiDataList); // 这里不使用move是适配一个packet解出多个frame的情况
            if (hwAccel_) {
                outFrame->setBackendHwType(hwAccel_->getBackendType());
            }

#ifdef VAAPI_AVAILABLE
            VASurfaceID surfaceID = VA_INVALID_ID;
            const auto curPixelFormat = outFrame->pixelFormat();
            if (curPixelFormat == AV_PIX_FMT_VAAPI) {
                surfaceID = (VASurfaceID)(uintptr_t)frame.data(3);
            }
#ifdef QSV_AVAILABLE
            if (curPixelFormat == AV_PIX_FMT_QSV) {
                mfxFrameSurface1 *const surface =
                    reinterpret_cast<mfxFrameSurface1 *>(frame.data(3));
                surfaceID = reinterpret_cast<VASurfaceID>(
                    reinterpret_cast<mfxHDLPair *>(surface->Data.MemId)->first);
            }
#endif
#endif

#ifdef VAAPI_AVAILABLE
            // 如果当前是vaapi, 则缓存surface
            // 并将导出的surface赋值给Frame
            if (vaDisplay) {
                // 同步surface
                va_wrapper::syncVASurface(vaDisplay, surfaceID);

                if (const auto iter = surfaceCache.find(surfaceID); iter == surfaceCache.end()) {
                    // surface不在缓存中，创建引用
                    // 导出surface
                    const auto desc = va_wrapper::exportVASurfaceHandle(vaDisplay, surfaceID);
                    // 创建VaapiSurfaceEGLExportData对应的AVBufferRef
                    auto *bufref = createVaapiSurfaceEGLExportDataBuffer(desc);
                    if (!bufref) {
                        LOG_WARN("Failed to create VaapiSurfaceEGLExportData buffer for surface {}",
                                 surfaceID);
                    } else {
                        surfaceCache.insert({surfaceID, bufref});
                        outFrame->attachVaapiSurfaceEGLExportData(bufref);
                    }
                } else {
                    outFrame->attachVaapiSurfaceEGLExportData(iter->second);
                }
            }
#endif

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

#ifdef VAAPI_AVAILABLE
    // 清理surface缓存
    clearSurfaceCache(surfaceCache);
#endif

    // 循环结束时，统计一次解码时间
    updateTotalDecodeTime();
}

HWAccelType VideoDecoder::initHwAccelContext()
{
    // 创建硬件加速器（默认尝试自动选择最佳硬件加速方式）
    hwAccel_ = HardwareAccelFactory::getInstance().createHardwareAccel(
        hwAccelType_, backendHwAccelType_, deviceIndex_, createHWContextCallback_,
        freeHWContextCallback_);
    if (!hwAccel_) {
        LOG_WARN("Hardware acceleration not available, using software decode");
        return HWAccelType::kNone;
    }

    return hwAccel_->getType();
}

bool VideoDecoder::setupHardwareDecode()
{
    needFixSPSProfile_ = false; // 重置SPS修正标志
    if (!hwAccel_)
        return false;

    if (hwAccel_->getType() == HWAccelType::kNone) {
        LOG_WARN("Hardware acceleration not available, using software decode");
        hwAccel_.reset();
        return false;
    } else {
        const std::string hardwareSource =
            hwAccel_->isUserContext()
                ? "user hardware context"
                : "device index " + std::to_string(hwAccel_->getDeviceIndex());
        LOG_INFO("Using hardware accelerator: {} ({}), source: {}", hwAccel_->getDeviceName(),
                 hwAccel_->getDeviceDescription(), hardwareSource);

        // 设置解码器上下文使用硬件加速
        if (!hwAccel_->setupDecoder(codecCtx_)) {
            LOG_WARN("Hardware acceleration setup failed, falling back to software");
            hwAccel_.reset();
            return false;
        }
    }

    // 如果创建的是D3D11、DXVA2类型的硬解码器，且是H264编码，则修复异常的SPS profile
    if ((hwAccel_->getType() == HWAccelType::kD3d11va ||
         hwAccel_->getType() == HWAccelType::kDxva2 ||
         hwAccel_->getType() == HWAccelType::kVaapi) &&
        codecCtx_->codec_id == AV_CODEC_ID_H264 && codecCtx_->extradata &&
        codecCtx_->extradata_size > 0) {
        needFixSPSProfile_ = fixH264ProfileIfNeeded(codecCtx_);
    }

    return true;
}

bool VideoDecoder::removeHardwareDecode()
{
    hwAccel_.reset();
    return true;
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

    int ret = 0;
    if (!hwAccel_->transferFrameToHost(hwFrame.get(), memoryFrame_.get(), &ret)) {
        handleDecodeTransError(ret);
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
        handleDecodeTransError(AVERROR_INVALIDDATA);
        return Frame();
    }

    // 设置目标帧参数
    swsFrame_.setPixelFormat(softPixelFormat_);
    swsFrame_.setWidth(frame.width());
    swsFrame_.setHeight(frame.height());
    swsFrame_.setAvPts(frame.avPts());
    swsFrame_.setMediaType(frame.mediaType());

    // 分配缓冲区
    int ret = av_frame_get_buffer(swsFrame_.get(), 0);
    if (ret < 0) {
        handleDecodeTransError(ret);
        return Frame();
    }

    // 执行转换
    ret = sws_scale(swsCtx_, (const uint8_t *const *)frame.get()->data, frame.get()->linesize, 0,
                    frame.height(), swsFrame_.get()->data, swsFrame_.get()->linesize);

    if (ret <= 0) {
        handleDecodeTransError(ret);
        return Frame();
    }

    // 复制帧属性
    av_frame_copy_props(swsFrame_.get(), frame.get());

    // 创建新Frame并移动，避免拷贝
    Frame result = std::move(swsFrame_);
    swsFrame_ = Frame(); // 重置为空，下次会重新分配
    return result;
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
           (errorCode == AVERROR_INVALIDDATA || errorCode == -1);
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

double VideoDecoder::calculateFrameDuration(const Frame &frame, double defaultDuration) const
{
    if (!frame.isValid())
        return 0.0;

    auto *avFrame = frame.get();
#if LIBAVUTIL_VERSION_MAJOR >= 58
    if (avFrame->duration > 0) {
        return avFrame->duration * av_q2d(stream_->time_base);
    }
#else
    if (avFrame->pkt_duration > 0) {
        return avFrame->pkt_duration * av_q2d(stream_->time_base);
    }
#endif
    /* if (lastPts >= 0 && avFrame->pts != AV_NOPTS_VALUE) {
         double dur = (avFrame->pts - lastPts) * av_q2d(stream_->time_base);
         return dur;
     }*/

    return defaultDuration;
}

void VideoDecoder::handleKeyFrameError(bool &hasKeyFrame, const std::string &errorString)
{
    // 丢弃整个GOP：清空解码器缓冲区
    avcodec_flush_buffers(codecCtx_);
    // 等待下一个关键帧到来
    hasKeyFrame = false;
    LOG_WARN("{}, url: {}", errorString, demuxer_->url());
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END