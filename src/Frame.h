#pragma once

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

class Frame {
public:
    Frame();
    explicit Frame(AVFrame *srcFrame);
    Frame(const Frame &other);
    Frame &operator=(const Frame &other);
    ~Frame();

    // 移动构造和移动赋值
    Frame(Frame &&other) noexcept;
    Frame &operator=(Frame &&other) noexcept;

    // 获得帧指针
    AVFrame *get() const;

    // 判断帧是否有效
    bool isValid() const;

    // ===================== 自定义数据 ===================== //
    // 获得序列号
    int serial() const;
    // 设置序列号
    void setSerial(int serial);

    // 获得帧时长
    double durationByFps() const;
    // 设置帧时长
    void setDurationByFps(double duration);

    // 是否是硬解码
    bool isInHardware() const;

    // 设置解码时间戳（单位s）
    void setSecPts(double sec);
    double secPts() const;
    // ==================================================== //

    // ====================== 数据透传 ===================== //
    // 图像尺寸
    int width() const;
    int height() const;
    void setWidth(int width);
    void setHeight(int height);

    // 像素格式
    AVPixelFormat pixelFormat() const;
    void setPixelFormat(AVPixelFormat format);
    // 时间戳相关
    int64_t avPts() const;
    void setAvPts(int64_t pts);

    int64_t pktDts() const;
    void setPktDts(int64_t dts);

    int64_t pktPos() const;
    void setPktPos(int64_t pos);

    int64_t pktSize() const;
    void setPktSize(int64_t size);

    // 时间基
    AVRational timeBase() const;
    void setTimeBase(AVRational tb);

    // 帧率
    AVRational sampleAspectRatio() const;
    void setSampleAspectRatio(AVRational sar);

    // 图像质量
    int quality() const;
    void setQuality(int quality);
    // 重复帧计数
    int repeatPict() const;
    void setRepeatPict(int repeat);

    // 隔行扫描
    int interlacedFrame() const;
    void setInterlacedFrame(int interlaced);

    int topFieldFirst() const;
    void setTopFieldFirst(int tff);

    // 图像类型
    AVPictureType pictType() const;
    void setPictType(AVPictureType type);
    // 关键帧标识
    int keyFrame() const;
    void setKeyFrame(int key);

    // 色彩空间
    AVColorSpace colorspace() const;
    void setColorspace(AVColorSpace cs);

    AVColorRange colorRange() const;
    void setColorRange(AVColorRange range);
    // 色度位置
    AVChromaLocation chromaLocation() const;
    void setChromaLocation(AVChromaLocation loc);

    // 推测时间戳
    int64_t bestEffortTimestamp() const;
    void setBestEffortTimestamp(int64_t ts);

    // 音频相关（如果是音频帧）
    int sampleRate() const;
    void setSampleRate(int rate);

    int nbSamples() const;
    void setNbSamples(int samples);

    AVSampleFormat sampleFormat() const;
    void setSampleFormat(AVSampleFormat fmt);

    // 声道布局（新版FFmpeg）
#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout channelLayout() const;
    void setChannelLayout(const AVChannelLayout &layout);
#else
    // 兼容旧版本
    uint64_t channelLayout() const;
    void setChannelLayout(uint64_t layout);
    int channels() const;
    void setChannels(int ch);
#endif

    // 数据访问方法
    uint8_t *data(int plane = 0) const;

    int linesize(int plane = 0) const;

    // 边信息访问
    AVFrameSideData *getSideData(AVFrameSideDataType type) const;
    AVFrameSideData *newSideData(AVFrameSideDataType type, int size);

    // 元数据访问
    AVDictionary *metadata() const;

    const char *getMetadata(const char *key) const;

    void setMetadata(const char *key, const char *value);

    // 实用方法
    bool isAudioFrame() const;

    bool isVideoFrame() const;

    // 获取帧的字节大小
    int getBufferSize() const;
    // ==================================================== //

    // 确保帧已分配
    void ensureAllocated();

    void release();
    void unref();

#ifdef USE_VAAPI
    bool copyFrmae(AVFrame *srcFrame);
#endif

private:
    friend class FrameQueue;

    // 帧
    AVFrame *frame_ = nullptr;
    // 序列号
    int serial_ = 0;
    // 帧的时长 单位s
    double duration_ = 0.0;
    // pts 单位s
    double pts_ = 0.0;
};