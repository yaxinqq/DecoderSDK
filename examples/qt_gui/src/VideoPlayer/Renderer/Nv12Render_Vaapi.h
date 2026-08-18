#pragma once
#ifdef VAAPI_AVAILABLE

#include "VideoRender.h"

#include <QMutex>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>

#include "decodersdk/vaapi_utils.h"

class Nv12Render_Vaapi : public VideoRender {
public:
    Nv12Render_Vaapi(QOpenGLContext *ctx);
    ~Nv12Render_Vaapi() override;

    /**
     * @brief 得到渲染器名称
     *
     * @return 渲染器名称
     */
    QString renderName() const override;

protected:
    /**
     * @brief 初始化VBO
     * @param horizontal 是否水平镜像
     * @param vertical 是否垂直镜像
     */
    bool initRenderVbo(const bool horizontal, const bool vertical) override;

    /**
     * @brief 初始化渲染Shader
     * @param frame 视频帧
     */
    bool initRenderShader(const decoder_sdk::Frame &frame) override;

    /**
     * @brief 初始化渲染纹理
     * @param frame 视频帧
     */
    bool initRenderTexture(const decoder_sdk::Frame &frame) override;

    /**
     * @brief 初始化硬件帧互操作资源
     * @param frame 视频帧
     */
    bool initInteropsResource(const decoder_sdk::Frame &frame) override;

    /**
     * @brief 将不同的硬件资源同一转换到OpenGL纹理
     *
     * @param frame 视频帧
     * @return 是否转换成功
     */
    bool interopToOpenGL(const decoder_sdk::Frame &frame) override;

    /**
     * @brief 渲染视频帧（当前转换后的资源），会绘制在当前绑定的FBO上
     *
     * @param 可能会用一些视频帧的数据，比如宽高
     * @return 是否渲染成功
     */
    bool renderToFbo(const decoder_sdk::Frame &frame) override;

    /**
     * @brief 清理渲染资源。会在OpenGL同步后调用，可以清理本轮次渲染视频帧的相关资源。
     */
    void cleanupRenderResources() override;

    /**
     * @brief 清理所有相关的资源（特定API资源 + 对应的OpenGL资源）
     */
    void cleanupAllResources() override;

private:
    /*
     * @brief 绘制视频帧
     *
     * @prarm idY Y纹理
     * @param idUV UV纹理
     */
    void drawFrame(GLuint idY, GLuint idUV);

    /**
     * @brief 清理EGL资源
     */
    void cleanupEGLTextures();

    /**
     * @brief 清理所有资源
     */
    void cleanup();

private:
    // EGL图像相关资源
    struct EGLImage {
        void *imageKHR = nullptr;
    };

    QMutex mtx_;
    QOpenGLShaderProgram program_;
    GLuint idY_ = 0, idUV_ = 0;
    QOpenGLBuffer vbo_;

    EGLImage yImage_;
    EGLImage uvImage_;

    // EGL上下文相关
    QVariant nativeEglHandle_;
};

#endif