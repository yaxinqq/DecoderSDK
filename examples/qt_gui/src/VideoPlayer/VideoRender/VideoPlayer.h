#pragma once
#include "CommonDef.h"

#include <QOpenGLWidget>
#include <QPointer>

class RenderWorker;
class VideoRender;
class QThread;
class VideoPlayerImpl;

class VideoPlayer : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit VideoPlayer(QWidget *parent = nullptr);
    virtual ~VideoPlayer();

public:
    // 暂停播放视频
    virtual void pause() = 0;
    // 恢复播放视频
    virtual void resume() = 0;

public:
    // 播放器播放状态
    Stream::PlayerState playerState() const;
    // 播放器宽高比模式
    Stream::AspectRatioMode aspectRatioMode() const;
    // 播放器矩形区域
    QRect widgetRect() const;
    QRect videoRect() const;

    // 流打开是否超时
    bool streamOpenTimeout() const;

    /*
     * @brief 将渲染器中的内容保存到图片
     *
     * @param image 保存的图片（OUT）
     */
    void renderToImage(QImage &image);

public:
    void setMasks(QList<QImage *> masks);
    void setShownScreenText(const QString &shownScreenText);

signals:
    void widgetRectChanged(const QRect &rect);
    void videoRectChanged(const QRect &rect);
    void playerStateChanged(Stream::PlayerState state);
    void totalTimeRecved(int totalTime);
    void ptsChanged(double pts);

    /*
     * @brief 流地址关闭，无法连接
     *
     */
    void streamClosed();

    /*
     * @brief 使播放器强制刷新（无新帧时也会刷新）。慎用，可能会引起卡顿
     */
    void forceToRender();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void paintEvent(QPaintEvent *e) override;

    // 设置播放状态（播放器内部使用），外部通过open、pause等接口设置播放状态
    void setPlayerState(Stream::PlayerState state);

    // 用来过滤播放器的显示/隐藏事件
    // bool event(QEvent* e) override;

protected:
    VideoPlayerImpl *impl_ = nullptr;

private:
    void aboutToRenderFrame();

private:
    // 窗口隐藏之前的播放状态
    Stream::PlayerState beforeHidePlayerState_;

    // 当前是否需要render frame
    std::atomic_bool needToRender_ = false;
};
