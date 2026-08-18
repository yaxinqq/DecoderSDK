#pragma once

#include "../Base/CommonDef.h"
#include "../Base/CommonUtils.h"

#include "decodersdk/frame.h"

#include <QObject>
#include <QFont>
#include <QImage>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QBindable>
#endif

class DisplayRenderer;
class RenderWorker;
class VideoRender;
class QThread;
class QOpenGLContext;
class QSurface;

/*!
 * \class VideoPlayerImpl
 *
 * \brief 播放器的内部实现类，所有和播放器有关的操作均在该类中实现
 *
 * \author ZYX
 * \date 2023/10/27
 */
class VideoPlayerImpl : public QObject
{
	Q_OBJECT

public:
	explicit VideoPlayerImpl(QObject* parent = nullptr);
	~VideoPlayerImpl();

	/*
     * @brief 获得player的uuid
     *
     */
    QString id() const;

public:
	/*
	 * @brief 初始化渲染器及其工作线程
	 *
	 * @param context 当前所用的openGL上下文
	 * @param surface 当前的绘制表面
	 */
	void initialize(QOpenGLContext* context, QSurface* surface);
	/*
	 * @brief 使用纯openGL的方法绘制渲染器中的纹理，这个方法需要渲染器所用的openGL上下文和当前容器的上下文是共享的
	 *
	 * @param widgetRect 容器大小
	 * @param needRenderedRect 需要被渲染区域的大小，区域的位置是相对于OpenGL窗口的（如QOpenGLWidget）,该区域坐标系是openGL坐标系，原点在区域左下点，x轴、y轴方向和世界坐标系相反
	 * @param referencePt OpenGL窗口的参考原点，
	 */
	void paintGL(const QRect& widgetRect, const QRect& needRenderedRect, const QPoint& referencePt);
	/*
	 * @brief 绘制普通的内容，比如帧数。光栅化显示
	 *
	 * @param painter 画笔
	 * @param widgetRect 容器大小
	 */
	void paintCommon(QPainter* painter, const QRect& widgetRect);
	/*
	 * @brief 容器重新改变大小
	 *
	 * @param w 容器的宽
	 * @param h 容器的高
	 */
	void resize(int w, int h);

	/*
	 * @brief 清屏，把屏幕刷成黑色，使用该方法之前，需保证OpenGL的上下文是有效的
	 *
     * @param needRenderedRect 需要被渲染区域的大小，区域的位置是相对于OpenGL窗口的（如QOpenGLWidget）,该区域坐标系是openGL坐标系，原点在区域左下点，x轴、y轴方向和世界坐标系相反
	 * @param referencePt OpenGL窗口的参考原点，
	 */
	void clear(const QRect& needRenderedRect, const QPoint& referencePt);

	/*
	 * @brief 通过painter的方法去清屏，把屏幕刷成黑色
	 *
	 * @param painter
	 * @param rect 需要刷成黑色的区域
	 */
	void clearByPainter(QPainter *painter, const QRect &rect);

public:
	// 播放器播放状态
	Stream::PlayerState playerState() { return playerState_; }
	// 播放器宽高比模式
	Stream::AspectRatioMode aspectRatioMode() const;
    void setAspectRatioMode(Stream::AspectRatioMode ratio);
	// 播放器矩形区域
	QRect widgetRect() const { return widgetRect_; }
	QRect videoRect() const { return videoRect_; }

public:
	void setShownScreenText(const QString& shownScreenText) { strText_ = shownScreenText; };

	/*
	 * @brief 将渲染器中的内容保存到图片
	 *
	 * @param size 帧缓冲的大小
	 * @param image 保存的图片（OUT）
	 */
	void renderToImage(const QSize& size, QImage& image);
	/*
	 * @brief 将渲染器中的内容保存到图片，将用视频帧的大小保存图片
	 *
	 * @param image 保存的图片（OUT）
	 */
	void renderToImage(QImage &image);

	// 设置播放状态（播放器内部使用），外部通过open、pause等接口设置播放状态
	void setPlayerState(Stream::PlayerState state);
	
	// 获得绘制文字的字体
	const QFont &textFont() const { return painterFont_; }

    /**
     * @brief 设置电子放大矩形
     *
     * @param rect 电子放大区域
     */
    void setDigitalZoomRect(const QRectF &rect);
    /**
     * @brief 获得当前电子放大的矩形区域
     *
     * @param rect 电子放大区域
     */
    QRectF digitalZoomRect() const;

	/**
     * @brief 设置是否水平翻转
     *
     * @param flip 是否水平翻转
     */
    void setHorizontalFlip(bool flip);
    /**
     * @brief 是否水平翻转
     *
     * @return 是否水平翻转
     */
    bool isHorizontalFlip() const;
    /**
     * @brief 设置是否垂直翻转
     *
     * @param flip 是否垂直翻转
     */
    void setVecticalFlip(bool flip);
    /**
     * @brief 是否垂直翻转
     *
     * @return 是否垂直翻转
     */
    bool isVecticalFlip() const;
    /**
     * @brief 设置是否水平、垂直翻转
     *
     * @param hFlip 是否水平翻转
     * @param vFlip 是否垂直翻转
     */
    void setHorizontalAndVecticalFlip(bool hflip, bool vflip);
    /**
     * @brief 是否水平翻转
     *
     * @return 是否水平翻转
     */
    QPair<bool, bool> isHorizontalAndVecticalFlip() const;

	/**
     * @brief 设置颜色调整值
     *
     * @param brightness 亮度
     * @param contrast 对比度
     * @param saturation 饱和度
     * @param hue 色调
     */
    void setColorAdjustments(float brightness, float contrast, float saturation, float hue);

    /**
     * @brief 设置亮度
     *
     * @param brightness 亮度
     */
    void setBrightness(float brightness);
    /**
     * @brief 获取亮度
     * @return 亮度
     */
    float brightness() const;

    /**
     * @brief 设置对比度
     *
     * @param contrast 对比度
     */
    void setContrast(float contrast);
    /**
     * @brief 获取对比度
     * @return 对比度
     */
    float contrast() const;

    /**
     * @brief 设置饱和度
     *
     * @param saturation 饱和度
     */
    void setSaturation(float saturation);
    /**
     * @brief 获取饱和度
     * @return 饱和度
     */
    float saturation() const;

    /**
     * @brief 设置色调
     *
     * @param hue 色调
     */
    void setHue(float hue);
    /**
     * @brief 获取色调
     * @return 色调
     */
    float hue() const;

public slots:
    void onDecoderEventChanged(const QString &url, decoder_sdk::EventType type,
                               const std::shared_ptr<decoder_sdk::EventArgs> &event);

    void onStreamInfoUpdated(const std::optional<decoder_sdk::StreamInfo> &info);
    void onStreamStaticsInfoUpdated(const decoder_sdk::StreamStaticsInfo &info);
    void onDecoderInfoUpdated(decoder_sdk::MediaType mediaType, const std::optional<decoder_sdk::DecoderInfo> &info);

	void onRendererNameChanged(const QString &name);
    void onTextureReady(const Stream::VideoFrameParam &videoFrameParam);
    void onDisplayRendererReady(const QString &playerId, std::weak_ptr<DisplayRenderer> renderer);
    void onDisplayRendererAboutToDestroy(const QString &playerId);

signals:
	void widgetRectChanged(const QRect& rect);
	void videoRectChanged(const QRect& rect);
	void playerStateChanged(Stream::PlayerState state);

	/**
	 * @brief 视频尺寸发生变化
	 * 
	 * @param width 宽
	 * @param height 高
	 */
    void frameSizeChanged(int width, int height);

	void totalTimeRecved(int64_t totalTime);
    void ptsChanged(double pts);

	/**
     * @brief 发生错误，该信号可能会多次重复的发出
	 * 
	 * @param errorType 错误类型
	 */
	void errorOccured(Stream::ErrorType errorType);

	/*
	 * @brief 录像已开启
	 *
	 */
	void recordStarted(const QString &filePath);
	/*
	 * @brief 录像已关闭
	 *
	 */
    void recordStopped(const QString &filePath);

	/*
	 * @brief 流地址关闭，无法连接
	 *
	 */
	void streamClosed();

    /*
     * @brief 文件流循环播放结束
     *
     */
    void fileStreamLoopEnded();

	/**
	 * @brief 跳转开始
	 */
	void seekStarted();
    /**
     * @brief 跳转结束
     */
    void seekEnded(bool success);

	void aboutToUpdate();

	/**
	 * @brief 即将被销毁，但如果是跨线程信号，则可能在收到信号时，此类已被销毁
	 */
	void aboutToDestroyed(const QString &uuid);

protected:
    void timerEvent(QTimerEvent *event) override;

private slots:
	void onPlayerStateChanged(Stream::PlayerState state);

private:
	/*
	 * @brief 计算视频帧显示区域
	 *
	 * @param needRenderedRect 需要被渲染的区域
	 */
	QRect calculateVideoRect(const QRect& needRenderedRect);

	/**
	 * @brief 组装统计信息
	 * 
	 */
	void setupStatisticalInfo();
    /**
     * @brief 重置统计信息延时更新的计时器
     *
     */
    void resetStatisticalInfoUpdateTimer();

	/**
	 * @brief 设置纹理尺寸
	 * 
	 * @param width 纹理宽度
	 * @param height 纹理高度
	 */
	void setFrameSize(int width, int height);

    /**
     * @brief 设置展示渲染器，调用此函数时，必须在主线程内！
     *
     * @param playerId 标明展示渲染器对应的是哪个player的
     * @param render 渲染器
     */
    void setDisplayRenderer(const QString &playerId, std::weak_ptr<DisplayRenderer> render = {});

private:
	std::weak_ptr<DisplayRenderer> render_;

	int frameWidth_ = 0;
	int frameHeight_ = 0;

	// FPS
	bool fpsVisible_ = false;
	int frameCount_ = 0;
	int fps_ = 0;
	int fpsTextWidth_;
	int fpsTextHeight_;

	QFont painterFont_;
	QString strText_;

	QRect widgetRect_;    // 容器尺寸
	QRect videoRect_;     // 视频的实际显示尺寸

	// 是否保持宽高比
    Stream::AspectRatioMode aspectRatio_ = Stream::AspectRatioMode::KeepAspectRatio;

	// 媒体信息
    std::optional<decoder_sdk::StreamInfo> streamInfo_;
	// 流统计信息（解码器生命周期内持续更新，如实时视频码率）
    decoder_sdk::StreamStaticsInfo streamStaticsInfo_;
	// 解码器信息
    QMap<decoder_sdk::MediaType, std::optional<decoder_sdk::DecoderInfo>> decoderInfos_;
	// 渲染器的名称
    QString rendererName_;
	
	// 统计信息
    QString statisticalInfo_;
	// 延时更新统计信息的计时器
    int updateStatisticalInfoTimerId_ = -1;

	// 视频处理参数
    Stream::VideoProcessParam processParam_;

	// 唯一标识，可用来标识录像文件
    const QString uuid_;

private:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	Q_OBJECT_BINDABLE_PROPERTY(VideoPlayerImpl, Stream::PlayerState, playerState_, &VideoPlayerImpl::playerStateChanged)
#else
	Stream::PlayerState playerState_;
#endif

};

