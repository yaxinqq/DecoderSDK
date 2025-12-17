#include "frame.h"

#ifdef VULKAN_AVAILABLE
extern "C" {
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_vulkan.h"
}
#endif

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

Frame::Frame()
    : frame_(nullptr), serial_(0), duration_(0), pts_(0.0), mediaType_(AVMEDIA_TYPE_UNKNOWN)
{
    // 不在构造函数中分配内存，只在需要时分配
}

Frame::Frame(AVFrame *srcFrame)
    : frame_(nullptr), serial_(0), duration_(0), pts_(0.0), mediaType_(AVMEDIA_TYPE_UNKNOWN)
{
    if (srcFrame) {
        ensureAllocated();
        if (av_frame_ref(frame_, srcFrame) != 0) {
            release();
        }
    }
}

Frame::Frame(const Frame &other)
    : frame_(nullptr),
      serial_(other.serial_),
      duration_(other.duration_),
      pts_(other.pts_),
      mediaType_(other.mediaType_),
      userSEIDataList_(other.userSEIDataList_),
      backendHwType_(other.backendHwType_)
{
    if (other.frame_) {
        ensureAllocated();
        if (av_frame_ref(frame_, other.frame_) != 0) {
            release();
        }
    }
}

Frame &Frame::operator=(const Frame &other)
{
    if (this != &other) {
        release();
        serial_ = other.serial_;
        duration_ = other.duration_;
        pts_ = other.pts_;
        mediaType_ = other.mediaType_;
        userSEIDataList_ = other.userSEIDataList_;
        backendHwType_ = other.backendHwType_;

        if (other.frame_) {
            ensureAllocated();
            if (av_frame_ref(frame_, other.frame_) != 0) {
                release();
            }
        }
    }
    return *this;
}

Frame::~Frame()
{
    release();
}

// 移动构造函数
Frame::Frame(Frame &&other) noexcept
    : frame_(other.frame_),
      serial_(other.serial_),
      duration_(other.duration_),
      pts_(other.pts_),
      mediaType_(other.mediaType_),
      userSEIDataList_(std::move(other.userSEIDataList_)),
      backendHwType_(other.backendHwType_)
{
    // 转移所有权，避免深拷贝
    other.frame_ = nullptr;
}

// 移动赋值运算符
Frame &Frame::operator=(Frame &&other) noexcept
{
    if (this != &other) {
        release();

        // 转移所有权
        frame_ = other.frame_;
        serial_ = other.serial_;
        duration_ = other.duration_;
        pts_ = other.pts_;
        mediaType_ = other.mediaType_;
        userSEIDataList_ = std::move(other.userSEIDataList_);
        backendHwType_ = other.backendHwType_;

        other.frame_ = nullptr;
    }
    return *this;
}

AVFrame *Frame::get() const
{
    return frame_;
}

bool Frame::isValid() const
{
    return frame_ != nullptr;
}

uint64_t Frame::serial() const
{
    return serial_;
}

void Frame::setSerial(uint64_t serial)
{
    serial_ = serial;
}

double Frame::durationByFps() const
{
    return duration_;
}

void Frame::setDurationByFps(double duration)
{
    duration_ = duration;
}

bool Frame::isInHardware() const
{
    return frame_ && frame_->hw_frames_ctx != nullptr;
}

void Frame::setSecPts(double pts)
{
    pts_ = pts;
}

double Frame::secPts() const
{
    return pts_;
}

AVMediaType Frame::mediaType() const
{
    return mediaType_;
}

void Frame::setMediaType(AVMediaType type)
{
    mediaType_ = type;
}

std::vector<UserSEIData> Frame::userSEIDataList() const
{
    return userSEIDataList_;
}

void Frame::setUserSEIDataList(const std::vector<UserSEIData> &seiDataList)
{
    userSEIDataList_ = seiDataList;
}

HWAccelType Frame::backendHwType() const
{
    return backendHwType_;
}

void Frame::setBackendHwType(HWAccelType hwType)
{
    if (!frame_)
        return;

    if (1
#ifdef QSV_AVAILABLE
        && frame_->format != AV_PIX_FMT_QSV
#endif
#ifdef AMF_AVAILABLE
        && frame_->format != AV_PIX_FMT_AMF_SURFACE
#endif
    ) {
        return;
    }

    backendHwType_ = hwType;
}

int Frame::width() const
{
    return frame_ ? frame_->width : 0;
}
int Frame::height() const
{
    return frame_ ? frame_->height : 0;
}
void Frame::setWidth(int width)
{
    if (frame_)
        frame_->width = width;
}
void Frame::setHeight(int height)
{
    if (frame_)
        frame_->height = height;
}

AVPixelFormat Frame::pixelFormat() const
{
    return frame_ ? static_cast<AVPixelFormat>(frame_->format) : AV_PIX_FMT_NONE;
}
void Frame::setPixelFormat(AVPixelFormat format)
{
    if (frame_)
        frame_->format = format;
}

int64_t Frame::avPts() const
{
    return frame_ ? frame_->pts : AV_NOPTS_VALUE;
}
void Frame::setAvPts(int64_t pts)
{
    if (frame_)
        frame_->pts = pts;
}

int64_t Frame::pktDts() const
{
    return frame_ ? frame_->pkt_dts : AV_NOPTS_VALUE;
}
void Frame::setPktDts(int64_t dts)
{
    if (frame_)
        frame_->pkt_dts = dts;
}

#if LIBAVUTIL_VERSION_MAJOR >= 57
AVRational Frame::timeBase() const
{
    return frame_ ? frame_->time_base : AVRational{0, 1};
}
void Frame::setTimeBase(AVRational tb)
{
    if (frame_)
        frame_->time_base = tb;
}
#endif

AVRational Frame::sampleAspectRatio() const
{
    return frame_ ? frame_->sample_aspect_ratio : AVRational{0, 1};
}
void Frame::setSampleAspectRatio(AVRational sar)
{
    if (frame_)
        frame_->sample_aspect_ratio = sar;
}

int Frame::quality() const
{
    return frame_ ? frame_->quality : 0;
}
void Frame::setQuality(int quality)
{
    if (frame_)
        frame_->quality = quality;
}

int Frame::repeatPict() const
{
    return frame_ ? frame_->repeat_pict : 0;
}
void Frame::setRepeatPict(int repeat)
{
    if (frame_)
        frame_->repeat_pict = repeat;
}

#if LIBAVUTIL_VERSION_MAJOR >= 58
int Frame::interlacedFrame() const
{
    return frame_ ? !!(frame_->flags & AV_FRAME_FLAG_INTERLACED) : 0;
}
void Frame::setInterlacedFrame(int interlaced)
{
    if (!frame_) {
        return;
    }

    if (interlaced)
        frame_->flags |= AV_FRAME_FLAG_INTERLACED;
    else
        frame_->flags &= ~AV_FRAME_FLAG_INTERLACED;
}

int Frame::topFieldFirst() const
{
    return frame_ ? !!(frame_->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) : 0;
}
void Frame::setTopFieldFirst(int tff)
{
    if (!frame_) {
        return;
    }

    if (tff)
        frame_->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
    else
        frame_->flags &= ~AV_FRAME_FLAG_TOP_FIELD_FIRST;
}
#endif

AVPictureType Frame::pictType() const
{
    return frame_ ? frame_->pict_type : AV_PICTURE_TYPE_NONE;
}
void Frame::setPictType(AVPictureType type)
{
    if (frame_)
        frame_->pict_type = type;
}

int Frame::keyFrame() const
{
#if LIBAVUTIL_VERSION_MAJOR >= 58
    return frame_ ? !!(frame_->flags & AV_FRAME_FLAG_KEY) : 0;
#else
    return frame_ ? frame_->key_frame : 0;
#endif
}
void Frame::setKeyFrame(int key)
{
    if (!frame_)
        return;

#if LIBAVUTIL_VERSION_MAJOR >= 58
    if (key)
        frame_->flags |= AV_FRAME_FLAG_KEY;
    else
        frame_->flags &= ~AV_FRAME_FLAG_KEY;
#else
    frame_->key_frame = !!(key);
#endif
}

AVColorSpace Frame::colorspace() const
{
    return frame_ ? frame_->colorspace : AVCOL_SPC_UNSPECIFIED;
}
void Frame::setColorspace(AVColorSpace cs)
{
    if (frame_)
        frame_->colorspace = cs;
}

AVColorRange Frame::colorRange() const
{
    return frame_ ? frame_->color_range : AVCOL_RANGE_UNSPECIFIED;
}
void Frame::setColorRange(AVColorRange range)
{
    if (frame_)
        frame_->color_range = range;
}

AVChromaLocation Frame::chromaLocation() const
{
    return frame_ ? frame_->chroma_location : AVCHROMA_LOC_UNSPECIFIED;
}
void Frame::setChromaLocation(AVChromaLocation loc)
{
    if (frame_)
        frame_->chroma_location = loc;
}

int64_t Frame::bestEffortTimestamp() const
{
    return frame_ ? frame_->best_effort_timestamp : AV_NOPTS_VALUE;
}

void Frame::setBestEffortTimestamp(int64_t ts)
{
    if (frame_)
        frame_->best_effort_timestamp = ts;
}

int Frame::sampleRate() const
{
    return frame_ ? frame_->sample_rate : 0;
}
void Frame::setSampleRate(int rate)
{
    if (frame_)
        frame_->sample_rate = rate;
}

int64_t Frame::nbSamples() const
{
    return frame_ ? frame_->nb_samples : 0;
}
void Frame::setNbSamples(int64_t samples)
{
    if (frame_)
        frame_->nb_samples = static_cast<int>(samples);
}

AVSampleFormat Frame::sampleFormat() const
{
    return frame_ ? static_cast<AVSampleFormat>(frame_->format) : AV_SAMPLE_FMT_NONE;
}
void Frame::setSampleFormat(AVSampleFormat fmt)
{
    if (frame_)
        frame_->format = fmt;
}

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100) // FFmpeg 5.1+
AVChannelLayout Frame::channelLayout() const
{
    return frame_ ? frame_->ch_layout : AVChannelLayout{};
}
void Frame::setChannelLayout(const AVChannelLayout &layout)
{
    if (frame_) {
        av_channel_layout_uninit(&frame_->ch_layout);
        av_channel_layout_copy(&frame_->ch_layout, &layout);
    }
}
#else
// 兼容旧版本
uint64_t Frame::channelLayout() const
{
    return frame_ ? frame_->channel_layout : 0;
}
void Frame::setChannelLayout(uint64_t layout)
{
    if (frame_)
        frame_->channel_layout = layout;
}

void Frame::setChannels(int ch)
{
    if (frame_)
        frame_->channels = ch;
}
#endif

int Frame::channels() const
{
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100) // FFmpeg 5.1+
    return frame_ ? frame_->ch_layout.nb_channels : 0;
#else
    return frame_ ? frame_->channels : 0;
#endif
}

uint8_t *Frame::data(int plane) const
{
    return (frame_ && plane < AV_NUM_DATA_POINTERS) ? frame_->data[plane] : nullptr;
}

int Frame::linesize(int plane) const
{
    return (frame_ && plane < AV_NUM_DATA_POINTERS) ? frame_->linesize[plane] : 0;
}

AVFrameSideData *Frame::getSideData(AVFrameSideDataType type) const
{
    return frame_ ? av_frame_get_side_data(frame_, type) : nullptr;
}

AVFrameSideData *Frame::newSideData(AVFrameSideDataType type, int size)
{
    return frame_ ? av_frame_new_side_data(frame_, type, size) : nullptr;
}

AVDictionary *Frame::metadata() const
{
    return frame_ ? frame_->metadata : nullptr;
}

const char *Frame::getMetadata(const char *key) const
{
    if (!frame_ || !frame_->metadata)
        return nullptr;
    AVDictionaryEntry *entry = av_dict_get(frame_->metadata, key, nullptr, 0);
    return entry ? entry->value : nullptr;
}

void Frame::setMetadata(const char *key, const char *value)
{
    if (frame_) {
        av_dict_set(&frame_->metadata, key, value, 0);
    }
}

int Frame::getBufferSize() const
{
    return frame_ ? av_image_get_buffer_size(pixelFormat(), width(), height(), 1) : 0;
}

int Frame::getAudioBufferSize() const
{
    return frame_ ? av_samples_get_buffer_size(nullptr, channels(), static_cast<int>(nbSamples()),
                                               sampleFormat(), 1)
                  : 0;
}

const VaapiSurfaceEGLExportData *const Frame::vaapiSurfaceEGLExportData() const
{
    if (!frame_ || !frame_->opaque_ref || pixelFormat() != AV_PIX_FMT_VAAPI)
        return nullptr;

    return reinterpret_cast<const VaapiSurfaceEGLExportData *const>(frame_->opaque_ref->data);
}

void Frame::attachVaapiSurfaceEGLExportData(AVBufferRef *externalBuf)
{
    if (!frame_ || pixelFormat() != AV_PIX_FMT_VAAPI || !externalBuf)
        return;

    // 先增加引用计数，保证外部 Buf 不会被提前释放
    AVBufferRef *bufRef = av_buffer_ref(externalBuf);
    if (!bufRef)
        return;

    // 替换旧的 opaque_ref
    if (frame_->opaque_ref) {
        av_buffer_unref(&frame_->opaque_ref);
    }

    frame_->opaque_ref = bufRef;
}

std::shared_ptr<VulkanFrame> Frame::lockVulkanFrame() const
{
    if (pixelFormat() != AV_PIX_FMT_VULKAN)
        return {};

#ifdef VULKAN_AVAILABLE
    AVHWFramesContext *frames = (AVHWFramesContext *)(frame_->hw_frames_ctx->data);
    AVVulkanFramesContext *vk = (AVVulkanFramesContext *)(frames->hwctx);
    AVVkFrame *pVkFrame = (AVVkFrame *)frame_->data[0];

    vk->lock_frame(frames, pVkFrame);

    std::shared_ptr<VulkanFrame> dst = std::make_shared<VulkanFrame>();
    constexpr size_t maxDstImages = sizeof(dst->img) / sizeof(dst->img[0]);
    constexpr size_t srcImages = AV_NUM_DATA_POINTERS;
    size_t count = (maxDstImages < srcImages) ? maxDstImages : srcImages;

    // 拷贝 VkImage 数组
    std::memcpy(dst->img, pVkFrame->img, count * sizeof(VkImage));

    // 拷贝 tiling
    dst->tiling = pVkFrame->tiling;

    // 拷贝 VkDeviceMemory 数组
    std::memcpy(dst->mem, pVkFrame->mem, count * sizeof(VkDeviceMemory));

    // 拷贝 size 数组
    std::memcpy(dst->size, pVkFrame->size, count * sizeof(size_t));

    // 拷贝 flags
    dst->flags = pVkFrame->flags;

    // 拷贝 access 数组
    std::memcpy(dst->access, pVkFrame->access, count * sizeof(VkAccessFlagBits));

    // 拷贝 layout 数组
    std::memcpy(dst->layout, pVkFrame->layout, count * sizeof(VkImageLayout));

    // 拷贝信号量数组
    std::memcpy(dst->sem, pVkFrame->sem, count * sizeof(VkSemaphore));

    // 拷贝 sem_value 数组
    std::memcpy(dst->sem_value, pVkFrame->sem_value, count * sizeof(uint64_t));

    // 拷贝 offset 数组
    std::memcpy(dst->offset, pVkFrame->offset, count * sizeof(ptrdiff_t));

    // 拷贝 queue_family 数组
    std::memcpy(dst->queue_family, pVkFrame->queue_family, count * sizeof(uint32_t));

    // 拷贝 format 数组
    std::memcpy(dst->format, vk->format, count * sizeof(VkFormat));

    // 拷贝 nb_layers
    dst->nb_layers = vk->nb_layers;

    // 保存AVVKFrame指针
    dst->avvkframePtr = frame_->data[0];
    return dst;
#else
    return {};
#endif
}

void Frame::unlockVulkanFrame(const std::shared_ptr<VulkanFrame> &frame) const
{
    if (pixelFormat() != AV_PIX_FMT_VULKAN)
        return;

#ifdef VULKAN_AVAILABLE
    AVHWFramesContext *frames = (AVHWFramesContext *)(frame_->hw_frames_ctx->data);
    AVVulkanFramesContext *vk = (AVVulkanFramesContext *)(frames->hwctx);
    AVVkFrame *pVkFrame = (AVVkFrame *)frame_->data[0];

    if (frame_->data[0] == frame->avvkframePtr) {
        constexpr size_t maxSrcImages = sizeof(frame->img) / sizeof(frame->img[0]);
        constexpr size_t dstImages = AV_NUM_DATA_POINTERS;
        size_t count = (maxSrcImages < dstImages) ? maxSrcImages : dstImages;

        // 拷贝 VkImage 数组
        std::memcpy(pVkFrame->img, frame->img, count * sizeof(VkImage));

        // 拷贝 tiling
        pVkFrame->tiling = frame->tiling;

        // 拷贝 VkDeviceMemory 数组
        std::memcpy(pVkFrame->mem, frame->mem, count * sizeof(VkDeviceMemory));

        // 拷贝 size 数组
        std::memcpy(pVkFrame->size, frame->size, count * sizeof(size_t));

        // 拷贝 flags
        pVkFrame->flags = frame->flags;

        // 拷贝 access 数组
        std::memcpy(pVkFrame->access, frame->access, count * sizeof(VkAccessFlagBits));

        // 拷贝 layout 数组
        std::memcpy(pVkFrame->layout, frame->layout, count * sizeof(VkImageLayout));

        // 拷贝信号量数组
        std::memcpy(pVkFrame->sem, frame->sem, count * sizeof(VkSemaphore));

        // 拷贝 sem_value 数组
        std::memcpy(pVkFrame->sem_value, frame->sem_value, count * sizeof(uint64_t));

        // 拷贝 offset 数组
        std::memcpy(pVkFrame->offset, frame->offset, count * sizeof(ptrdiff_t));

        // 拷贝 queue_family 数组
        std::memcpy(pVkFrame->queue_family, frame->queue_family, count * sizeof(uint32_t));
    }

    vk->unlock_frame(frames, pVkFrame);

#endif
}

void Frame::lockVulkanQueue(uint32_t queue_family, uint32_t index) const
{
    if (pixelFormat() != AV_PIX_FMT_VULKAN)
        return;

#ifdef VULKAN_AVAILABLE
    AVHWFramesContext *frames = (AVHWFramesContext *)(frame_->hw_frames_ctx->data);
    AVHWDeviceContext *deviceCtx = frames->device_ctx;
    AVVulkanDeviceContext *vulkanContext = (AVVulkanDeviceContext *)deviceCtx->hwctx;

    vulkanContext->lock_queue(deviceCtx, queue_family, index);
#endif
}

void Frame::unlockVulkanQueue(uint32_t queue_family, uint32_t index) const
{
    if (pixelFormat() != AV_PIX_FMT_VULKAN)
        return;

#ifdef VULKAN_AVAILABLE
    AVHWFramesContext *frames = (AVHWFramesContext *)(frame_->hw_frames_ctx->data);
    AVHWDeviceContext *deviceCtx = frames->device_ctx;
    AVVulkanDeviceContext *vulkanContext = (AVVulkanDeviceContext *)deviceCtx->hwctx;

    vulkanContext->unlock_queue(deviceCtx, queue_family, index);
#endif
}

void Frame::ensureAllocated()
{
    if (!frame_) {
        frame_ = av_frame_alloc();
    }
}

void Frame::release()
{
    if (frame_) {
        unref();
        av_frame_free(&frame_);
        frame_ = nullptr;
    }
}

void Frame::unref()
{
    if (frame_) {
        av_frame_unref(frame_);
    }
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END