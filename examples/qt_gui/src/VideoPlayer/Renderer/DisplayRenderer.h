#pragma once
#include "../Base/CommonDef.h"

#include <QImage>
#include <QMutex>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QSharedPointer>

class QOpenGLContext;

class DisplayRenderer : public QOpenGLFunctions {
public:
    DisplayRenderer(const QString &uuid);
    ~DisplayRenderer();

    QString id() const;

    // RenderWorker调用
public:
    /**
     * @brief 初始化渲染器，分配VBO，编译链接shader等
     *
     * @param context 当前的OpenGL上下文
     */
    void initialize();

    /**
     * @brief 是否初始化完成
     *
     * @return 是否完成初始化
     */
    bool isInitialized() const;

    /**
     * @brief 清理当前的fbo，用在Renderer析构时，在OpenGLContext done之前，先将资源释放
     */
    void cleanup();

    // VideoPlayerImpl调用
public:
    /**
     * @brief 使用当前帧的处理参数绘制
     * @param param 当前帧的视频处理参数
     */
    void draw(const Stream::VideoProcessParam &param);

    /**
     * @brief 将图像渲染到缓存帧中，外部负责释放QOpenGLFramebufferObject
     *
     * @param param 当前帧的视频处理参数
     * @return 当前显示的FBO，如果没有可用的FBO则返回nullptr
     */
    QSharedPointer<QOpenGLFramebufferObject> getFrameBuffer(const Stream::VideoProcessParam &param);

    /**
     * @brief 将当前纹理转到图片
     *
     * @param param 当前帧的视频处理参数
     * @param size 图片的大小
     * @param image 图片
     * @return
     */
    void currentFrameToImage(const Stream::VideoProcessParam &param, const QSize &size, QImage &image);

private:
    // VideoRender调用
    /**
     * @brief 获得可用的后台纹理，可能为空。由于只会被VideoRender调用，这里不加锁
     *
     * @return 后台纹理的智能指针
     */
    QSharedPointer<QOpenGLFramebufferObject> acquireAvailableBackendBuffer(const QSize &size,
                                                                           const QOpenGLFramebufferObjectFormat &fmt);

    /**
     * @brief 交换前台和后台的纹理缓冲对象，用于上层的渲染
     *
     */
    void swap();

private:
    /**
     * @brief 初始化FBO绘制资源
     * @retur 是否成功
     */
    bool initializeFboDrawResources();

    /**
     * @brief 绘制FBO到屏幕
     * @param fbo 要绘制的FBO
     * @param param 当前帧使用的视频处理参数快照
     */
    void drawFbo(QSharedPointer<QOpenGLFramebufferObject> fbo, const Stream::VideoProcessParam &param);

    /**
     * @brief 创建一个FBO
     * @param size FBO的大小
     * @param fmt FBO的格式
     * @return 创建的FBO指针
     */
    QSharedPointer<QOpenGLFramebufferObject> createFbo(const QSize &size,
                                                       const QOpenGLFramebufferObjectFormat &fmt);

    /**
     * @brief 把给定的rect转换到OpenGL坐标系下，并保存为vec4，便于传给着色器
     *
     * @param rect 给定的矩形区域
     * @param needTrans 是否需要转换y值
     * @return OpenGL坐标系下的vec4
     */
    QVector4D transToOpenGLUniform(const QRectF &rect, bool needTrans = true) const;

    /**
     * @brief 得到翻转参数
     *
     * @param hFlip 是否水平翻转
     * @param vFlip 是否垂直翻转
     * @return 翻转向量（x：水平、y：垂直）
     */
    QVector2D getFlipParam(bool hFlip, bool vFlip) const;

    /**
     * @brief 通过globalShareContext获得当前线程中的上下文，用来做为兜底
     *
     * @return 全局共享上下文在当前线程中的上下文
     */
    QOpenGLContext *sharedContext();

private:
    friend class VideoRender;

    // fbo的锁，交换和渲染可能不在一个线程
    QMutex mtx_;
    // 当前(前台)的纹理
    QSharedPointer<QOpenGLFramebufferObject> curFbo_;
    // 后台纹理，用于交换
    QSharedPointer<QOpenGLFramebufferObject> nextFbo_;

    // 用于绘制FBO到屏幕的资源
    QOpenGLShaderProgram fboDrawProgram_;
    QOpenGLBuffer fboDrawVbo_;
    std::atomic_bool fboDrawResourcesInitialized_;
    int textureUniformLocation_ = -1;
    int zoomRectUniformLocation_ = -1;
    int texFlipUniformLocation_ = -1;
    int brightnessUniformLocation_ = -1;
    int contrastUniformLocation_ = -1;
    int saturationUniformLocation_ = -1;
    int hueUniformLocation_ = -1;

    // 唯一标识符，和VideoPlayerImpl的uuid一一对应
    const QString uuid_;
};
