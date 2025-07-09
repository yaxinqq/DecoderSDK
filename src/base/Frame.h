#ifndef DECODER_SDK_INTERNAL_FRAME_H
#define DECODER_SDK_INTERNAL_FRAME_H
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

#include "base_define.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class Frame {
public:
    /**
     * @brief 构造函数
     */
    Frame();
    /**
     * @brief 构造函数
     * @param srcFrame 源AVFrame指针
     */
    explicit Frame(AVFrame *srcFrame);
    /**
     * @brief 拷贝构造函数
     * @param other 要拷贝的Frame对象
     */
    Frame(const Frame &other);
    /**
     * @brief 拷贝赋值函数
     * @param other 要拷贝的Frame对象
     * @return Frame& 拷贝后的Frame对象
     */
    Frame &operator=(const Frame &other);
    /**
     * @brief 析构函数
     */
    ~Frame();

    /**
     * @brief 移动构造函数
     * @param other 要移动的Frame对象
     */
    Frame(Frame &&other) noexcept;
    /**
     * @brief 移动赋值函数
     * @param other 要移动的Frame对象
     * @return Frame& 移动后的Frame对象
     */
    Frame &operator=(Frame &&other) noexcept;

    /**
     * @brief 获得帧指针
     * @return AVFrame* 帧指针
     */
    AVFrame *get() const;

    /**
     * @brief 判断帧是否有效
     * @return true 有效, false 无效
     */
    bool isValid() const;

    // ===================== 自定义数据 ===================== //
    /**
     * @brief 获得序列号
     * @return int 序列号
     */
    int serial() const;
    /**
     * @brief 设置序列号
     * @param serial 序列号
     */
    void setSerial(int serial);

    /**
     * @brief 获得帧时长
     * @return double 帧时长
     */
    double durationByFps() const;
    /**
     * @brief 设置帧时长
     * @param duration 帧时长
     */
    void setDurationByFps(double duration);

    /**
     * @brief 是否是硬解码
     * @return true 是, false 否
     */
    bool isInHardware() const;

    /**
     * @brief 设置解码时间戳（单位s）
     * @param sec 解码时间戳
     */
    void setSecPts(double sec);
    /**
     * @brief 获得解码时间戳（单位s）
     * @return double 解码时间戳
     */
    double secPts() const;
    // ==================================================== //

    // ====================== 数据透传 ===================== //
    /**
     * @brief 获得图像宽度
     * @return int 图像宽度
     */
    int width() const;
    /**
     * @brief 获得图像高度
     * @return int 图像高度
     */
    int height() const;
    /**
     * @brief 设置图像宽度
     * @param width 图像宽度
     */
    void setWidth(int width);
    /**
     * @brief 设置图像高度
     * @param height 图像高度
     */
    void setHeight(int height);

    /**
     * @brief 获得像素格式
     * @return AVPixelFormat 像素格式
     */
    AVPixelFormat pixelFormat() const;
    /**
     * @brief 设置像素格式
     * @param format 像素格式
     */
    void setPixelFormat(AVPixelFormat format);

    /**
     * @brief 获得AV展示时间戳
     * @return int64_t AV展示时间戳
     */
    int64_t avPts() const;
    /**
     * @brief 设置AV展示时间戳
     * @param pts AV展示时间戳
     */
    void setAvPts(int64_t pts);

    /**
     * @brief 获得AV解码时间戳
     * @return int64_t AV解码时间戳
     */
    int64_t pktDts() const;
    /**
     * @brief 设置AV解码时间戳
     * @param dts AV解码时间戳
     */
    void setPktDts(int64_t dts);

#if LIBAVUTIL_VERSION_MAJOR >= 57
    /**
     * @brief 获得时间基
     * @return AVRational 时间基
     */
    AVRational timeBase() const;
    /**
     * @brief 设置时间基
     * @param tb 时间基
     */
    void setTimeBase(AVRational tb);
#endif

    /**
     * @brief 获得帧率
     * @return AVRational 帧率
     */
    AVRational sampleAspectRatio() const;
    /**
     * @brief 设置帧率
     * @param sar 帧率
     */
    void setSampleAspectRatio(AVRational sar);

    /**
     * @brief 获得图像质量
     * @return int 图像质量
     */
    int quality() const;
    /**
     * @brief 设置图像质量
     * @param quality 图像质量
     */
    void setQuality(int quality);

    /**
     * @brief 获得重复帧计数
     * @return int 重复帧计数
     */
    int repeatPict() const;
    /**
     * @brief 设置重复帧计数
     * @param repeat 重复帧计数
     */
    void setRepeatPict(int repeat);

#if LIBAVUTIL_VERSION_MAJOR >= 58
    /**
     * @brief 获得隔行扫描
     * @return int 隔行扫描
     */
    int interlacedFrame() const;
    /**
     * @brief 设置隔行扫描
     * @param interlaced 隔行扫描
     */
    void setInterlacedFrame(int interlaced);

    /**
     * @brief 获得顶部字段优先
     * @return int 顶部字段优先
     */
    int topFieldFirst() const;
    /**
     * @brief 设置顶部字段优先
     * @param tff 顶部字段优先
     */
    void setTopFieldFirst(int tff);
#endif

    /**
     * @brief 获得图像类型
     * @return AVPictureType 图像类型
     */
    AVPictureType pictType() const;
    /**
     * @brief 设置图像类型
     * @param type 图像类型
     */
    void setPictType(AVPictureType type);

    /**
     * @brief 获得关键帧标识
     * @return int 关键帧标识
     */
    int keyFrame() const;
    /**
     * @brief 设置关键帧标识
     * @param key 关键帧标识
     */
    void setKeyFrame(int key);

    /**
     * @brief 获得色彩空间
     * @return AVColorSpace 色彩空间
     */
    AVColorSpace colorspace() const;
    /**
     * @brief 设置色彩空间
     * @param cs 色彩空间
     */
    void setColorspace(AVColorSpace cs);

    /**
     * @brief 获得色彩范围
     * @return AVColorRange 色彩范围
     */
    AVColorRange colorRange() const;
    /**
     * @brief 设置色彩范围
     * @param range 色彩范围
     */
    void setColorRange(AVColorRange range);

    /**
     * @brief 获得色度位置
     * @return AVChromaLocation 色度位置
     */
    AVChromaLocation chromaLocation() const;
    /**
     * @brief 设置色度位置
     * @param loc 色度位置
     */
    void setChromaLocation(AVChromaLocation loc);

    /**
     * @brief 获得推测时间戳
     * @return int64_t 推测时间戳
     */
    int64_t bestEffortTimestamp() const;
    /**
     * @brief 设置推测时间戳
     * @param ts 推测时间戳
     */
    void setBestEffortTimestamp(int64_t ts);

    /**
     * @brief 获得采样率
     * @return int 采样率
     */
    int sampleRate() const;
    /**
     * @brief 设置采样率
     * @param rate 采样率
     */
    void setSampleRate(int rate);

    /**
     * @brief 获得样本数量
     * @return int 样本数量
     */
    int64_t nbSamples() const;
    /**
     * @brief 设置样本数量
     * @param samples 样本数量
     */
    void setNbSamples(int64_t samples);

    /**
     * @brief 获得样本格式
     * @return AVSampleFormat 样本格式
     */
    AVSampleFormat sampleFormat() const;
    /**
     * @brief 设置样本格式
     * @param fmt 样本格式
     */
    void setSampleFormat(AVSampleFormat fmt);

    // 声道布局（新版FFmpeg）
#if LIBAVUTIL_VERSION_MAJOR >= 57
    /**
     * @brief 获得声道布局
     * @return AVChannelLayout 声道布局
     */
    AVChannelLayout channelLayout() const;
    /**
     * @brief 设置声道布局
     * @param layout 声道布局
     */
    void setChannelLayout(const AVChannelLayout &layout);
#else
    // 兼容旧版本
    uint64_t channelLayout() const;
    void setChannelLayout(uint64_t layout);
    int channels() const;
    void setChannels(int ch);
#endif

    /**
     * @brief 获得数据指针
     * @param plane 平面索引
     * @return uint8_t* 数据指针
     */
    uint8_t *data(int plane = 0) const;

    /**
     * @brief 获得行大小
     * @param plane 平面索引
     * @return int 行大小
     */
    int linesize(int plane = 0) const;

    /**
     * @brief 获得边信息
     * @param type 边信息类型
     * @return AVFrameSideData* 边信息
     */
    AVFrameSideData *getSideData(AVFrameSideDataType type) const;
    /**
     * @brief 创建边信息
     * @param type 边信息类型
     * @param size 边信息大小
     * @return AVFrameSideData* 边信息
     */
    AVFrameSideData *newSideData(AVFrameSideDataType type, int size);

    /**
     * @brief 获得元数据
     * @return AVDictionary* 元数据
     */
    AVDictionary *metadata() const;
    /**
     * @brief 获得元数据
     * @param key 元数据键
     * @return const char* 元数据值
     */
    const char *getMetadata(const char *key) const;
    /**
     * @brief 设置元数据
     * @param key 元数据键
     * @param value 元数据值
     */
    void setMetadata(const char *key, const char *value);

    /**
     * @brief 获得是否音频帧
     * @return true 音频帧, false 视频帧
     */
    bool isAudioFrame() const;
    /**
     * @brief 获得是否视频帧
     * @return true 视频帧, false 音频帧
     */
    bool isVideoFrame() const;

    /**
     * @brief 获取帧的字节大小
     * @return int 帧的字节大小
     */
    int getBufferSize() const;
    // ==================================================== //

    /**
     * @brief 确保帧已分配
     */
    void ensureAllocated();

    /**
     * @brief 释放帧
     */
    void release();
    /**
     * @brief 减少引用计数
     */
    void unref();

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

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_FRAME_H