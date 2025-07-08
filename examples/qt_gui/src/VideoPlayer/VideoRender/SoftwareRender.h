#ifndef SOFTWAREVIDEORENDER_H
#define SOFTWAREVIDEORENDER_H

#include "VideoRender.h"
#include "decodersdk/common_define.h"
#include "decodersdk/frame.h"

#include <QMutex>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

class SoftwareRender : public VideoRender {
public:
    SoftwareRender();
    ~SoftwareRender() override;

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
    // 初始化相关
    bool initializeShaders(decoder_sdk::ImageFormat format);
    void cleanup();

    // 纹理处理
    bool uploadYUVTextures(const decoder_sdk::Frame &frame);
    bool uploadRGBTexture(const decoder_sdk::Frame &frame);
    bool createTextures(int width, int height, decoder_sdk::ImageFormat format);
    void clearTextures();

    // 着色器相关
    const char *getVertexShader() const;
    const char *getFragmentShader(decoder_sdk::ImageFormat format) const;

    // 格式支持检查
    bool isSupportedFormat(decoder_sdk::ImageFormat format) const;
    bool isYUVFormat(decoder_sdk::ImageFormat format) const;
    bool isRGBFormat(decoder_sdk::ImageFormat format) const;

    // OpenGL错误检查
    bool checkGLError(const char *operation);
    
    // 纹理对象
    struct TextureSet {
        GLuint yTexture = 0;  // Y分量或RGB纹理
        GLuint uTexture = 0;  // U分量纹理
        GLuint vTexture = 0;  // V分量纹理
        GLuint uvTexture = 0; // UV交错纹理(NV12/NV21)
    };
    /*
     * @brief 绘制视频帧
     *
     * @prarm textures 纹理组
     * @prarm format 图片格式
     */
    bool drawFrame(const TextureSet &textures, decoder_sdk::ImageFormat format);

private:
    // OpenGL资源
    QOpenGLShaderProgram program_;
    QOpenGLBuffer vbo_;

    // 当前纹理
    TextureSet textures_;

    // 状态标志
    bool texturesCreated_ = false;
};

#endif // SOFTWAREVIDEORENDER_H