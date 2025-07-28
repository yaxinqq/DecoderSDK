#include "frame.h"

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
      mediaType_(other.mediaType_)
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
      mediaType_(other.mediaType_)
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

int Frame::serial() const
{
    return serial_;
}

void Frame::setSerial(int serial)
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

#if LIBAVUTIL_VERSION_MAJOR >= 57
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

int Frame::channels() const
{
    return frame_ ? frame_->channels : 0;
}
void Frame::setChannels(int ch)
{
    if (frame_)
        frame_->channels = ch;
}
#endif

int Frame::channels() const
{
#if LIBAVUTIL_VERSION_MAJOR >= 57
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
    return frame_ ? av_samples_get_buffer_size(nullptr, channels(), static_cast<int>(nbSamples()), sampleFormat(), 1)
                  : 0;
}

void Frame::ensureAllocated()
{
    if (!frame_) {
        frame_ = av_frame_alloc();
    }
}

void Frame::unref()
{
    if (frame_) {
        av_frame_unref(frame_);
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

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END