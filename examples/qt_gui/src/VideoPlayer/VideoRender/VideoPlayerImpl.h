#pragma once
#include "../CommonDef.h"

#include "decodersdk/frame.h"

#include <QFont>
#include <QImage>
#include <QObject>

class RenderWorker;
class VideoRender;
class QThread;
class QOpenGLContext;
class QSurface;
struct AVFrame;

/*!
 * \class VideoPlayerImpl
 *
 * \brief 播放器的内部实现类，所有和播放器有关的操作均在该类中实现
 *
 * \author ZYX
 * \date 2023/10/27
 */
class VideoPlayerImpl : public QObject {
    Q_OBJECT

public:
    explicit VideoPlayerImpl(QObject *parent = nullptr);
    ~VideoPlayerImpl();

public:
    /*
     * @brief 初始化渲染器及其工作线程
     *
     * @param context 当前所用的openGL上下文
     * @param surface 当前的绘制表面
     */
    void initialize(QOpenGLContext *context, QSurface *surface);
    /*
     * @brief
     * 使用纯openGL的方法绘制渲染器中的纹理，这个方法需要渲染器所用的openGL上下文和当前容器的上下文是共享的
     *
     * @param widgetRect 容器大小
     * @param needRenderedRect
     * 需要被渲染区域的大小，区域的位置是相对于OpenGL窗口的（如QOpenGLWidget）,该区域坐标系是openGL坐标系，原点在区域左下点，x轴、y轴方向和世界坐标系相反
     * @param referencePt OpenGL窗口的参考原点，
     */
    void paintGL(const QRect &widgetRect, const QRect &needRenderedRect, const QPoint &referencePt);
    /*
     * @brief 绘制普通的内容，比如帧数。光栅化显示
     *
     * @param painter 画笔
     * @param widgetRect 容器大小
     */
    void paintCommon(QPainter *painter, const QRect &widgetRect);
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
     * @param needRenderedRect
     * 需要被渲染区域的大小，区域的位置是相对于OpenGL窗口的（如QOpenGLWidget）,该区域坐标系是openGL坐标系，原点在区域左下点，x轴、y轴方向和世界坐标系相反
     * @param referencePt OpenGL窗口的参考原点，
     */
    void clear(const QRect &needRenderedRect, const QPoint &referencePt);

    /*
     * @brief 通过painter的方法去清屏，把屏幕刷成黑色
     *
     * @param painter
     * @param rect 需要刷成黑色的区域
     */
    void clearByPainter(QPainter *painter, const QRect &rect);

public:
    // 播放器播放状态
    Stream::PlayerState playerState()
    {
        return playerState_;
    }
    // 播放器宽高比模式
    Stream::AspectRatioMode aspectRatioMode() const;
    // 播放器矩形区域
    QRect widgetRect() const
    {
        return widgetRect_;
    }
    QRect videoRect() const
    {
        return videoRect_;
    }

    // 流打开是否超时
    bool streamOpenTimeout() const
    {
        return streamOpenTimeout_.load();
    }

public:
    void videoFrameReady(const std::shared_ptr<decoder_sdk::Frame>& frame);

    void setMasks(QList<QImage *> masks)
    {
        masks_ = masks;
    }
    void setShownScreenText(const QString &shownScreenText)
    {
        strText_ = shownScreenText;
    };

    /*
     * @brief 将渲染器中的内容保存到图片
     *
     * @param size 帧缓冲的大小
     * @param image 保存的图片（OUT）
     */
    void renderToImage(const QSize &size, QImage &image);
    /*
     * @brief 将渲染器中的内容保存到图片，将用视频帧的大小保存图片
     *
     * @param image 保存的图片（OUT）
     */
    void renderToImage(QImage &image);

    // 设置播放状态（播放器内部使用），外部通过open、pause等接口设置播放状态
    void setPlayerState(Stream::PlayerState state);

    // 获得绘制文字的字体
    const QFont &textFont() const
    {
        return painterFont_;
    }

public slots:
    void onDecoderEventChanged(decoder_sdk::EventType type,
                               const std::shared_ptr<decoder_sdk::EventArgs> &event);

signals:
    void renderRequested(const std::shared_ptr<decoder_sdk::Frame>& frame);
    void prepareStop();
    void preparePause();
    void preparePlaying();
    void widgetRectChanged(const QRect &rect);
    void videoRectChanged(const QRect &rect);
    void playerStateChanged(Stream::PlayerState state);

    void totalTimeRecved(int totalTime);
    void ptsChanged(double pts);

    /*
     * @brief 录像已开启
     *
     */
    void recordStarted();
    /*
     * @brief 录像已关闭
     *
     */
    void recordStopped();
    /*
     * @brief 录像时发生了错误
     *
     */
    void recordErrorOccured();

    /*
     * @brief 流地址关闭，无法连接
     *
     */
    void streamClosed();

    void aboutToUpdate();

private slots:
    void onPlayerStateChanged(Stream::PlayerState state);

private:
    /*
     * @brief 计算视频帧显示区域
     *
     * @param needRenderedRect 需要被渲染的区域
     */
    QRect calculateVideoRect(const QRect &needRenderedRect);

private:
    RenderWorker *renderWorker_ = nullptr;
    QWeakPointer<VideoRender> render_;
    QThread *renderWorkerThread_ = nullptr;

    int frameWidth_ = -1;
    int frameHeight_ = -1;

    // FPS
    bool fpsVisible_ = true;
    int frameCount_ = 0;
    int fps_ = 0;
    int fpsTextWidth_;
    int fpsTextHeight_;

    QFont painterFont_;
    QString strText_;
    QList<QImage *> masks_;

    QRect widgetRect_; // 容器尺寸
    QRect videoRect_;  // 视频的实际显示尺寸

    // 暂停前的最后一帧（图像的原始大小）
    QImage lastFrame_;

    // 流地址打开失败的计时器，用于记录超时时间
    QTimer *streamOpenErrorTimer_ = nullptr;
    // 流地址是否打开超时
    std::atomic_bool streamOpenTimeout_;

private:
    Stream::PlayerState playerState_;
};
