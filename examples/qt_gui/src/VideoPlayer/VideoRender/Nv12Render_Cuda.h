#pragma once
#ifdef CUDA_AVAILABLE

#include "../CommonUtils.h"
#include "VideoRender.h"

#include <QMutex>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>

#include <condition_variable>

class Nv12Render_Cuda : public VideoRender {
public:
    Nv12Render_Cuda();
    ~Nv12Render_Cuda() override;

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
     * @brief 渲染视频帧，会绘制在一个FBO上
     * @param frame 视频帧
     */
    bool renderFrame(const decoder_sdk::Frame &frame) override;

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
     * @brief 清除互操作资源
     */
    void cleanupCudaResource();
    /**
     * @brief 清除OpenGL资源
     */
    void cleanupOpenGLResource();

    /**
     * @brief 映射CUDA资源
     *
     * @return 是否映射成功
     */
    bool mapCudaResource();
    /**
     * @brief 取消映射CUDA资源
     *
     * @return 是否取消映射成功
     */
    bool unmapCudaResource();

private:
    // CUDA的上下文和流
    CUcontext context_ = nullptr;
    CUstream copyStream_ = nullptr;

    // CUDA的资源映射对象
    CUgraphicsResource resourceY_ = nullptr;
    CUgraphicsResource resourceUV_ = nullptr;

    // 用来从CUDA中拷贝数据
    CUarray cudaArrayY_ = nullptr;
    CUarray cudaArrayUV_ = nullptr;

    // 资源是否已映射到OpenGL上下文
    bool resourceYMapped_ = false;
    bool resourceUVMapped_ = false;

    // OpenGL的相关对象
    QOpenGLShaderProgram program_;
    QOpenGLBuffer vbo_;
    GLuint idY_ = 0, idUV_ = 0;
};

#endif
