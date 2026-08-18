#pragma once

#include "../Base/CommonDef.h"

#include <QOpenGLWidget>
#include <QPointer>
#include <QScopedPointer>
#include <QVector>

#include <optional>

class RenderWorker;
class VideoRender;
class QThread;
class VideoPlayerImpl;
class QMouseEvent;
class RubberBand;
class QWheelEvent;
struct AVFrame;

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

    /*
     * @brief 将渲染器中的内容保存到图片
     *
     * @param image 保存的图片（OUT）
     */
    void renderToImage(QImage &image);

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
     * @brief 获得当前最大电子放大倍率
     *
     * @return 当前最大电子放大倍率
     */
    int maxDigitalZoomFactor() const;
    /**
     * @brief 设置当前最大电子放大倍率
     *
     * @param maxFactor 最大倍率
     */
    void setMaxDigitalZoomFactor(int maxFactor);
    /**
     * @brief 设置黑名单区域，当鼠标点中这些区域后，不进行电子放大处理
     *
     * @param rects 黑名单区域
     */
    void setBlackRects(const QVector<QRect> &rects);
    /**
     * @brief 设置是否启用鼠标电子放大控制
     *
     * @param enabled 是否启用
     */
    void setDigitalZoomControlEnabled(bool enabled);
    /**
     * @brief 当前是否启用鼠标电子放大控制
     *
     * @return 是否启用
     */
    bool isDigitalZoomControlEnabled() const;

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

    /*
     * @brief 开启录像
     *
     * @param recodDir 保存录像的目录
     */
    virtual void startRecoding(const QString &recodDir);

    /*
     * @brief 停止录像
     *
     */
    virtual void stopRecoding();

    /*
     * @brief 是否正在录像
     */
    virtual bool isRecording() const;

    /*
     * @brief 获得默认录像文件名称
     *
     */
    QString defaultRecordFileName() const;

public:
    void setShownScreenText(const QString &shownScreenText);

signals:
    void widgetRectChanged(const QRect &rect);
    void videoRectChanged(const QRect &rect);
    void playerStateChanged(Stream::PlayerState state);
    void totalTimeRecved(int64_t totalTime);
    void ptsChanged(double pts);

    /**
     * @brief 视频尺寸发生变化
     *
     * @param width 宽
     * @param height 高
     */
    void frameSizeChanged(int width, int height);

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

    /*
     * @brief 使播放器强制刷新（无新帧时也会刷新）。慎用，可能会引起卡顿
     */
    void forceToRender();

    /**
     * @brief 发生错误，该信号可能会多次重复的发出
     *
     * @param errorType 错误类型
     */
    void errorOccured(Stream::ErrorType errorType);

    /*
     * @brief 录像已开启
     *
     * @param filePath 录制文件路径
     */
    void recordStarted(const QString filePath);
    /*
     * @brief 录像已关闭
     *
     * @param filePath 录制文件路径
     */
    void recordStopped(const QString filePath);

    /**
     * @brief 跳转开始
     */
    void seekStarted();
    /**
     * @brief 跳转结束
     */
    void seekEnded(bool success);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

    // 设置播放状态（播放器内部使用），外部通过open、pause等接口设置播放状态
    void setPlayerState(Stream::PlayerState state);

protected:
    VideoPlayerImpl *impl_ = nullptr;

private:
    void aboutToRenderFrame();

    QRect acceptedDigitalZoomResponseRect() const;
    QPoint constrainPointToRect(const QPoint &point, const QRect &rect) const;
    bool inDigitalZoomBlackRect(const QPoint &point) const;
    void setNormalizedDigitalZoomRect(QRectF rect);
    void setDigitalZoomScale(double scale);
    void moveDigitalZoomRect(int xStep, int yStep);
    void resetDigitalZoomRect();
    void resetDigitalZoomInteractionState();

private:
    // 当前是否需要render frame
    std::atomic_bool needToRender_ = false;

    // ================== 电子放大相关 ================== //
    // 是否启用鼠标电子放大控制，默认关闭以避免影响现有交互
    bool digitalZoomControlEnabled_ = false;
    // 鼠标左键按下
    bool leftBtnPressed_ = false;
    // 鼠标右键按下
    bool rightBtnPressed_ = false;
    // 画框基准位置
    std::optional<QPoint> originPos_ = std::nullopt;
    // 电子放大-移动时，右键按下时的坐标
    QPoint pressedPos_;
    // 滚轮缩放系数
    double wheelFactor_ = 0.8;
    // 最大电子放大倍率
    int maxDigitalZoomFactor_ = 40;
    // 鼠标事件黑名单区域，命中后直接忽略事件以便父级处理
    QVector<QRect> blackRects_;
    // 画框橡皮筋
    QScopedPointer<RubberBand, QScopedPointerDeleteLater> rubberBand_;
    // ================================================== //
};
