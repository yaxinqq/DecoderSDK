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

class FboQueue;
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
     * @brief 渲染
     */
    void render(const std::shared_ptr<decoder_sdk::Frame> &frame);

    /**
     * @brief 绘制
     */
    void draw();

    /**
     * @brief 将图像渲染到缓存帧中，外部负责释放QOpenGLFramebufferObject
     */
    QSharedPointer<QOpenGLFramebufferObject> getFrameBuffer();

    /*
     * @brief render是否有效，目前通过是否完成初始化来判断
     *
     * @return 是否有效
     */
    bool isValid() const;

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

    /**
     * @brief 同步OpenGL，并轮询当前任务是否完成，会阻塞当前线程，直到OpenGL命令执行完成
     * @param sleepInterval 睡眠间隔，单位毫秒，默认5毫秒
     */
    void syncOpenGL(int sleepInterval = 5);

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

private:
    // 纹理交换锁
    QMutex mutex_;

    // 处于后台更新状态的FBO
    QSharedPointer<QOpenGLFramebufferObject> nextFbo_;
    // 处于绘制状态的FBO
    QSharedPointer<QOpenGLFramebufferObject> curFbo_;

    // 用于绘制FBO到屏幕的资源
    QOpenGLShaderProgram fboDrawProgram_;
    QOpenGLBuffer fboDrawVbo_;
    std::atomic_bool fboDrawResourcesInitialized_;

    // 是否初始化完成
    std::atomic_bool initialized_;
    // 是否支持glFence
    bool supportsGlFence_ = false;
};

#endif // VIDEORENDER_H