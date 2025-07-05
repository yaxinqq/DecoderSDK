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

class SoftwareRender : public QOpenGLFunctions, public VideoRender {
public:
    SoftwareRender();
    ~SoftwareRender() override;

    // VideoRender接口实现
    void initialize(const decoder_sdk::Frame &frame, const bool horizontal = false,
                    const bool vertical = false) override;
    void render(const decoder_sdk::Frame &frame) override;
    void draw() override;
    QOpenGLFramebufferObject *getFrameBuffer(const QSize &size) override;

private:
    // 初始化相关
    bool initializeShaders(decoder_sdk::ImageFormat format);
    bool initializeVertexBuffer();
    void cleanup();

    // 纹理处理
    bool uploadYUVTextures(const decoder_sdk::Frame &frame);
    bool uploadRGBTexture(const decoder_sdk::Frame &frame);
    bool createTextures(int width, int height, decoder_sdk::ImageFormat format);
    void clearTextures();
    void swapTextures();

    // 着色器相关
    const char *getVertexShader() const;
    const char *getFragmentShader(decoder_sdk::ImageFormat format) const;

    // 格式支持检查
    bool isSupportedFormat(decoder_sdk::ImageFormat format) const;
    bool isYUVFormat(decoder_sdk::ImageFormat format) const;
    bool isRGBFormat(decoder_sdk::ImageFormat format) const;

    // OpenGL错误检查
    bool checkGLError(const char *operation);

private:
    // 纹理同步锁
    QMutex mtx_;

    // OpenGL资源
    QOpenGLShaderProgram program_;
    QOpenGLBuffer vbo_;
    std::unique_ptr<QOpenGLFramebufferObject> fbo_;

    // 双缓冲纹理对象 - Current用于显示，Next用于上传
    struct TextureSet {
        GLuint yTexture = 0;  // Y分量或RGB纹理
        GLuint uTexture = 0;  // U分量纹理
        GLuint vTexture = 0;  // V分量纹理
        GLuint uvTexture = 0; // UV交错纹理(NV12/NV21)
    };

    TextureSet currentTextures_; // 当前显示的纹理
    TextureSet nextTextures_;    // 下一帧准备的纹理

    // 渲染参数
    int videoWidth_ = 0;
    int videoHeight_ = 0;
    bool flipHorizontal_ = false;
    bool flipVertical_ = false;
    decoder_sdk::ImageFormat currentFormat_ = decoder_sdk::ImageFormat::kUnknown;

    // 状态标志
    bool initialized_ = false;
    bool texturesCreated_ = false;
};

#endif // SOFTWAREVIDEORENDER_H