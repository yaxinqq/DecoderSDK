#include "Nv12Render_Vaapi.h"

#ifdef VAAPI_AVAILABLE
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <QtPlatformHeaders/QEGLNativeContext>

#include "../Commonutils.h"

namespace {
const char *vsrc = R"(
#ifdef GL_ES
    precision mediump float;
#endif
	
    attribute vec4 vertexIn;
	attribute vec2 textureIn;
	varying vec2 textureOut;
	void main(void)
	{
		gl_Position = vertexIn;
		textureOut = textureIn;
	}
)";

const char *fsrc = R"(
#ifdef GL_ES
    precision mediump float;
#endif

    uniform sampler2D textureY;
    uniform sampler2D textureUV;

    varying vec2 textureOut;

    void main(void)
    {
        // 采样Y和UV纹理
        float y = texture2D(textureY, textureOut).r;
        vec2 uv = texture2D(textureUV, textureOut).rg;

        // 常量偏移和转换矩阵
        const vec3 yuv2rgb_ofs = vec3(0.0625, 0.5, 0.5);
        const mat3 yuv2rgb_mat = mat3(
            1.16438356,  0.0,           1.79274107,
            1.16438356, -0.21324861, -0.53290932,
            1.16438356,  2.11240178,  0.0
        );

        // YUV到RGB的转换
        vec3 rgb = (vec3(y, uv.r, uv.g) - yuv2rgb_ofs) * yuv2rgb_mat;
        gl_FragColor = vec4(rgb, 1.0);
    }
)";
} // namespace

inline bool checkError(const char* msg,int iLine, const char* szFile) {
    EGLint err = eglGetError();
    if (err != EGL_SUCCESS) {
        qDebug() << "ERROR: " << msg << err << " at line " << iLine << " in file " << szFile;
        return false;
    }
    return true;
}
#define ck(call) checkError(call, __LINE__, __FILE__)

Nv12Render_Vaapi::Nv12Render_Vaapi(QOpenGLContext *ctx) 
    : VideoRender()
    , vaDisplay_(vaapi_utils::getVADisplayDRM())
{
    if (ctx && ctx->isValid()) {
        nativeEglHandle_ = ctx->nativeHandle();
        if (!nativeEglHandle_.canConvert<QEGLNativeContext>()) {
            qWarning() << QStringLiteral("Can not get eglContext!");
        }
    }
}

Nv12Render_Vaapi::~Nv12Render_Vaapi()
{	
    cleanupEGLTextures();

    for (auto *id: {&idY_, &idUV_}) {
        glDeleteTextures(1, id);
    }

    vbo_.destroy();
}

bool Nv12Render_Vaapi::initRenderVbo(const bool horizontal, const bool vertical)
{
    initDefaultVBO(vbo_, horizontal, vertical);
    return true;
}

bool Nv12Render_Vaapi::initRenderShader(const decoder_sdk::Frame &frame)
{
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vsrc);
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fsrc);
    program_.link();

    return true;
}

bool Nv12Render_Vaapi::initRenderTexture(const decoder_sdk::Frame &frame)
{
    // 纹理和缓冲的初始值
    const auto width = frame.width();
    const auto height = frame.height();
    const qopengl_GLsizeiptr frameSize =
        static_cast<qopengl_GLsizeiptr>(width) * static_cast<qopengl_GLsizeiptr>(height);
    const std::vector<unsigned char> initYData(frameSize, 0);
    const std::vector<unsigned char> initUVData(frameSize / 2, 128);

    // 初始化纹理 Y
    glGenTextures(1, &idY_);
    glBindTexture(GL_TEXTURE_2D, idY_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE,
                 initYData.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // 初始化纹理 UV
    glGenTextures(1, &idUV_);
    glBindTexture(GL_TEXTURE_2D, idUV_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG, width >> 1, height >> 1, 0, GL_RG, GL_UNSIGNED_BYTE,
                 initUVData.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

bool Nv12Render_Vaapi::initInteropsResource(const decoder_sdk::Frame &frame)
{
    return true;
}

bool Nv12Render_Vaapi::renderFrame(const decoder_sdk::Frame &frame)
{
    QEGLNativeContext eglContext = nativeEglHandle_.value<QEGLNativeContext>();

    if (!frame.isValid() || !vaDisplay_ || !eglContext.context()) {
        clearGL();
        return false;
    }

    cleanupEGLTextures();

    const VASurfaceID surfaceID = (VASurfaceID)(uintptr_t)frame.data(3);
    decoder_sdk::syncVASurface(vaDisplay_, surfaceID);

    // zero copy
    const auto desc = decoder_sdk::exportVASurfaceHandle(vaDisplay_, surfaceID);
    
    EGLint yAttrs[] = {
        EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(desc.layers[0].drm_format),
        EGL_WIDTH, frame.width(),
        EGL_HEIGHT, frame.height(),
        EGL_DMA_BUF_PLANE0_FD_EXT, desc.objects[0].fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, desc.layers[0].offset[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, desc.layers[0].pitch[0],
        EGL_NONE
    };
    yImage_.imageKHR = egl::egl_create_image_KHR(eglContext.display(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL, yAttrs);
    if (!yImage_.imageKHR) {
        qWarning() << QStringLiteral("egl_create_image_KHR to create yImageKHR failed!");
    }
    yImage_.fd = desc.objects[0].fd;

    EGLint uvAttrs[] = {
        EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(desc.layers[1].drm_format),
        EGL_WIDTH, frame.width() / 2,
        EGL_HEIGHT, frame.height() / 2,
        EGL_DMA_BUF_PLANE0_FD_EXT, desc.objects[0].fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, desc.layers[1].offset[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, desc.layers[1].pitch[0],
        EGL_NONE
    };
    uvImage_.imageKHR = egl::egl_create_image_KHR(eglContext.display(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL, uvAttrs);
    if (!uvImage_.imageKHR) {
        qWarning() << QStringLiteral("egl_create_image_KHR to create uvImageKHR failed!");
    }
    uvImage_.fd = desc.objects[0].fd;

    glBindTexture(GL_TEXTURE_2D, idY_);
    egl::gl_egl_image_target_texture2d_oes(GL_TEXTURE_2D, yImage_.imageKHR);

    glBindTexture(GL_TEXTURE_2D, idUV_);
    egl::gl_egl_image_target_texture2d_oes(GL_TEXTURE_2D, uvImage_.imageKHR);

    // 绘制
    drawFrame(idY_, idUV_);
    return true;
}

void Nv12Render_Vaapi::cleanupRenderResources()
{
    cleanupEGLTextures();
}

void Nv12Render_Vaapi::drawFrame(GLuint idY, GLuint idUV)
{
    clearGL();

    program_.bind();
    vbo_.bind();
    program_.enableAttributeArray("vertexIn");
    program_.enableAttributeArray("textureIn");
    program_.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 2, 0);
    program_.setAttributeBuffer("textureIn", GL_FLOAT, 2 * 4 * sizeof(GLfloat), 2, 0);

    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, idY);

    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, idUV);

    program_.setUniformValue("textureY", 1);
    program_.setUniformValue("textureUV", 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    program_.disableAttributeArray("vertexIn");
    program_.disableAttributeArray("textureIn");
    vbo_.release();
    program_.release();
}

void Nv12Render_Vaapi::cleanupEGLTextures()
{
    QEGLNativeContext eglContext = nativeEglHandle_.value<QEGLNativeContext>();
    const auto clearFunction = [](EGLImage &image, const QEGLNativeContext &eglContext){
        if (image.imageKHR && eglContext.display()) {
            egl::egl_destroy_image_KHR(eglContext.display(), image.imageKHR);
        }
        if (image.fd >= 0) {
            ::close(image.fd);
        }
        image.imageKHR = nullptr;
        image.fd = -1;
    };

    clearFunction(yImage_, eglContext);
    clearFunction(uvImage_, eglContext);
}

#endif