#pragma once
#ifdef CUDA_AVAILABLE

#include "VideoRender.h"

#include <QMutex>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>

#include <cuda.h>
#include <cudaGL.h>

#include <condition_variable>

class Nv12Render_Cuda : public VideoRender {
public:
    Nv12Render_Cuda();
    ~Nv12Render_Cuda() override;

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

private:
    /*
     * @brief 绘制视频帧
     *
     * @prarm idY Y纹理
     * @param idUV UV纹理
     */
    void drawFrame(GLuint idY, GLuint idUV);

private:
    // CUDA流同步
    std::condition_variable conditional_;
    std::mutex conditionalMtx_;

    std::atomic_bool copyYSucced_ = false;
    std::atomic_bool copyUVSucced_ = false;

    // CUDA的上下文和流
    CUcontext context_ = nullptr;
    CUstream copyYStream_ = nullptr;
    CUstream copyUVStream_ = nullptr;

    // CUDA的资源映射对象
    CUgraphicsResource resourceY_ = nullptr;
    CUgraphicsResource resourceUV_ = nullptr;

    // 用来从CUDA中拷贝数据
    CUarray cudaArrayY_ = nullptr;
    CUarray cudaArrayUV_ = nullptr;

    // 资源映射是否成功
    bool resourceYRegisteredFailed_ = false;
    bool resourceUVRegisteredFailed_ = false;

    // OpenGL的相关对象
    QOpenGLShaderProgram program_;
    QOpenGLBuffer vbo_;
    GLuint idY_ = 0, idUV_ = 0;
};

#endif