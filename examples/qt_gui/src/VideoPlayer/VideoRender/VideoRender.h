#ifndef VIDEORENDER_H
#define VIDEORENDER_H

#include "decodersdk/frame.h"

#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QScopedPointer>
#include <QSharedPointer>

class FboQueue;
class VideoRender : protected QOpenGLFunctions {
public:
    VideoRender();
    virtual ~VideoRender();

    /**
     * @description:
     * 初始化OpenGL上下文，编译链接shader；如果是GPU直接与OpenGL对接数据，则会分配GPU内存或注册资源
     * @param frame		 视频帧
     * @param horizontal 是否水平镜像
     * @param vertical	 是否垂直镜像
     */
    void initialize(const decoder_sdk::Frame &frame, const bool horizontal = false,
                    const bool vertical = false);

    /**
     * @description: 渲染
     */
    void render(const decoder_sdk::Frame &frame);

    /**
     * @description: 绘制
     */
    void draw();

    /**
     * @description: 将图像渲染到缓存帧中，外部负责释放QOpenGLFramebufferObject
     */
    QSharedPointer<QOpenGLFramebufferObject> getFrameBuffer();

protected:
    /**
     * @description: 初始化VBO
     * @param horizontal 是否水平镜像
     * @param vertical 是否垂直镜像
     */
    virtual void initRenderVbo(const bool horizontal, const bool vertical) = 0;

    /**
     * @description: 初始化渲染Shader
     * @param frame 视频帧
     */
    virtual void initRenderShader(const decoder_sdk::Frame &frame) = 0;

    /**
     * @description: 初始化渲染纹理
     * @param frame 视频帧
     */
    virtual void initRenderTexture(const decoder_sdk::Frame &frame) = 0;

    /**
     * @description: 初始化硬件帧互操作资源
     * @param frame 视频帧
     */
    virtual void initInteropsResource(const decoder_sdk::Frame &frame) = 0;

    /**
     * @description: 渲染视频帧，会绘制在一个FBO上
     * @param frame 视频帧
     */
    virtual void renderFrame(const decoder_sdk::Frame &frame) = 0;

private:
    void initializeFboDrawResources();
    void drawFboToScreen(QSharedPointer<QOpenGLFramebufferObject> fbo);
    QSharedPointer<QOpenGLFramebufferObject> createFbo(const QSize &size,
                                                       const QOpenGLFramebufferObjectFormat &fmt);
    void updateCurrentFbo(QSharedPointer<QOpenGLFramebufferObject> newFbo);

private:
    // FBO队列
    QScopedPointer<FboQueue> fboQueue_;

    // 当前需处于绘制状态的FBO
    QSharedPointer<QOpenGLFramebufferObject> curFbo_;

    // 用于绘制FBO到屏幕的资源
    QOpenGLShaderProgram fboDrawProgram_;
    QOpenGLBuffer fboDrawVbo_;
    bool initialized_;
    bool fboDrawResourcesInitialized_;
};

#endif // VIDEORENDER_H