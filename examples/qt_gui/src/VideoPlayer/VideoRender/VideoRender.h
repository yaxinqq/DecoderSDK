#ifndef VIDEORENDER_H
#define VIDEORENDER_H
#include "decodersdk/frame.h"

#include <QMutex>
#include <QOpenGLBuffer>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QScopedPointer>
#include <QSharedPointer>
#include <memory>

class VideoRender : protected QOpenGLExtraFunctions {
public:
    VideoRender();
    virtual ~VideoRender();

    /**
     * @brief
     * 初始化OpenGL上下文，编译链接shader；如果是GPU直接与OpenGL对接数据，则会分配GPU内存或注册资源
     * @param frame		 视频帧
     * @param horizontal 是否水平镜像
     * @param vertical	 是否垂直镜像
     */
    void initialize(const std::shared_ptr<decoder_sdk::Frame> &frame, const bool horizontal = false,
                    const bool vertical = false);

    /**
     * @brief 渲染（非阻塞）
     * @param frame 视频帧
     */
    void render(const std::shared_ptr<decoder_sdk::Frame> &frame);

    /**
     * @brief 绘制
     */
    void draw();

    /**
     * @brief 将图像渲染到缓存帧中，外部负责释放QOpenGLFramebufferObject
     * @return 当前显示的FBO，如果没有可用的FBO则返回nullptr
     */
    QSharedPointer<QOpenGLFramebufferObject> getFrameBuffer();

    /*
     * @brief render是否有效，目前通过是否完成初始化来判断
     *
     * @return 是否有效
     */
    bool isValid() const;

    /*
     * @brief render是否需要重建
     *
     * @return 是否需要重建。目前仅D3D11va和DXVA2有重建需求
     */
    virtual bool shouldRebuild() const;

    /**
     * @brief 得到渲染器名称
     *
     * @return 渲染器名称
     */
    virtual QString renderName() const = 0;

public:
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

protected:
    /**
     * @brief 初始化VBO
     * @param horizontal 是否水平镜像
     * @param vertical 是否垂直镜像
     */
    virtual bool initRenderVbo(const bool horizontal, const bool vertical) = 0;

    /**
     * @brief 初始化渲染Shader
     * @param frame 视频帧
     */
    virtual bool initRenderShader(const decoder_sdk::Frame &frame) = 0;

    /**
     * @brief 初始化渲染纹理
     * @param frame 视频帧
     */
    virtual bool initRenderTexture(const decoder_sdk::Frame &frame) = 0;

    /**
     * @brief 初始化硬件帧互操作资源
     * @param frame 视频帧
     */
    virtual bool initInteropsResource(const decoder_sdk::Frame &frame) = 0;

    /**
     * @brief 渲染视频帧，会绘制在一个FBO上
     * @param frame 视频帧
     */
    virtual bool renderFrame(const decoder_sdk::Frame &frame) = 0;

    /**
     * @brief 清理渲染资源。会在OpenGL同步后调用，可以清理本轮次渲染视频帧的相关资源。
     */
    virtual void cleanupRenderResources()
    {
    }

protected:
    /*
     * @brief 创建一个默认的VBO，其中的顶点坐标和纹理坐标，分离式存储
     *        前四组（x、y）是顶点坐标，后四组（x、y)是纹理坐标
     */
    void initDefaultVBO(QOpenGLBuffer &vbo, const bool horizontal, const bool vertical) const;

    /*
     * @brief OpenGL清屏
     */
    void clearGL();

protected:
    bool isIntelGpu_ = false;

private:
    /**
     * @brief 初始化FBO绘制资源
     * @param size FBO的大小
     * @retur 是否成功
     */
    bool initializeFboDrawResources(const QSize &size);

    /**
     * @brief 绘制FBO到屏幕
     * @param fbo 要绘制的FBO
     */
    void drawFbo(QSharedPointer<QOpenGLFramebufferObject> fbo);

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

private:
    QMutex mtx_;
    QSharedPointer<QOpenGLFramebufferObject> curFbo_;
    QSharedPointer<QOpenGLFramebufferObject> nextFbo_;

    // 用于绘制FBO到屏幕的资源
    QOpenGLShaderProgram fboDrawProgram_;
    QOpenGLBuffer fboDrawVbo_;
    std::atomic_bool fboDrawResourcesInitialized_;

    // 是否初始化完成
    std::atomic_bool initialized_;
    // 是否支持glFence
    bool supportsGlFence_ = false;
    // 是否强制GPU等待
    bool forceGpuFinish_ = false;

    // 电子放大区域
    QRectF digitalZoomRect_;
};

#endif // VIDEORENDER_H