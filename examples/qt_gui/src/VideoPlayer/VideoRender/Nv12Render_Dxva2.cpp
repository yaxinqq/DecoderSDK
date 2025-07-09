#ifdef DXVA2_AVAILABLE
#include "Nv12Render_Dxva2.h"
#include "Commonutils.h"

#ifdef _WIN32
#include <Windows.h>
#include <d3d9.h>
#include <dxva2api.h>
#endif

#include <QDebug>
#include <iostream>

namespace {
// 顶点着色器源码
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

// RGB纹理渲染的片段着色器
const char *fsrc = R"(
#ifdef GL_ES
    precision mediump float;
#endif

    uniform sampler2D texture0;
    varying vec2 textureOut;
    void main(void)
    {
        gl_FragColor = texture2D(texture0, textureOut);
    }
)";
} // namespace

Nv12Render_Dxva2::Nv12Render_Dxva2()
    : VideoRender()
    , d3d9Device_(dxva2_utils::getDXVA2Device())
{

}

Nv12Render_Dxva2::~Nv12Render_Dxva2()
{
    cleanup();

    // 清理WGL设备
    if (wglD3DDevice_) {
        wglDXCloseDeviceNV(wglD3DDevice_);
        wglD3DDevice_ = nullptr;
    }

    // 清理VBO
    vbo_.destroy();
}

bool Nv12Render_Dxva2::initRenderVbo(const bool horizontal, const bool vertical)
{
    initDefaultVBO(vbo_, horizontal, vertical);
    return true;
}

bool Nv12Render_Dxva2::initRenderShader(const decoder_sdk::Frame &frame)
{
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vsrc);
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fsrc);
    program_.link();
    
    return true;
}

bool Nv12Render_Dxva2::initRenderTexture(const decoder_sdk::Frame &frame)
{
    if (!createRgbRenderTarget())
        return false;

    return true;
}

bool Nv12Render_Dxva2::initInteropsResource(const decoder_sdk::Frame &frame)
{
    if (!initializeWGLInterop()) {
        qWarning() << "[Nv12Render_Dxva2] Failed to initialize WGL interop!";
        return false;
    }

    if (!registerTextureWithOpenGL(frame.width(), frame.height())) {
        qWarning() << "[Nv12Render_Dxva2] Failed to register D3D texture to OpenGL texture!";
        return false;
    }

    return true;
}

bool Nv12Render_Dxva2::renderFrame(const decoder_sdk::Frame &frame)
{
    if (!frame.isValid()) {
        return false;
    }

    // 从Frame中提取DXVA2表面指针
    LPDIRECT3DSURFACE9 sourceSurface = reinterpret_cast<LPDIRECT3DSURFACE9>(frame.data(3));
    if (!sourceSurface) {
        qWarning() << "[Nv12Render_Dxva2] Invalid DXVA2 surface!";
        return false;
    }

    // 使用StretchRect转换NV12到RGB
    if (!convertNv12ToRgbStretchRect(sourceSurface)) {
        qWarning() << "[Nv12Render_Dxva2] Failed to convert NV12 to RGB!";
        return false;
    }

    // 绘制
    return drawFrame(sharedTexture_);
}

bool Nv12Render_Dxva2::initializeWGLInterop()
{
    if (!loadWGLExtensions()) {
        qWarning() << "[Nv12Render_Dxva2] Failed to load WGL extensions!";
        return false;
    }

    if (!d3d9Device_) {
        qWarning() << "[Nv12Render_Dxva2] D3D9 device is null!";
        return false;
    }

    HRESULT hr = d3d9Device_->TestCooperativeLevel();
    if (FAILED(hr)) {
        qWarning() << "[Nv12Render_Dxva2] D3D9 device is in error state, HRESULT:" << hr;
        return false;
    }

    wglD3DDevice_ = wglDXOpenDeviceNV(d3d9Device_.Get());
    if (!wglD3DDevice_) {
        DWORD error = GetLastError();
        qWarning() << "[Nv12Render_Dxva2] Failed to open D3D device for WGL interop, error:" << error;
        return false;
    }

    return true;
}

void Nv12Render_Dxva2::cleanup()
{
    if (wglTextureHandle_ && wglD3DDevice_) {
        wglDXUnregisterObjectNV(wglD3DDevice_, wglTextureHandle_);
        wglTextureHandle_ = nullptr;
    }

    if (sharedTexture_) {
        glDeleteTextures(1, &sharedTexture_);
        sharedTexture_ = 0;
    }

    if (rgbRenderTarget_) {
        rgbRenderTarget_.Reset();
    }
}

bool Nv12Render_Dxva2::loadWGLExtensions()
{
    // 加载函数指针
    wglDXOpenDeviceNV = (PFNWGLDXOPENDEVICENVPROC)wglGetProcAddress("wglDXOpenDeviceNV");
    wglDXCloseDeviceNV = (PFNWGLDXCLOSEDEVICENVPROC)wglGetProcAddress("wglDXCloseDeviceNV");
    wglDXSetResourceShareHandleNV = (PFNWGLDXSETRESOURCESHAREHANDLENVPROC)wglGetProcAddress(
        "wglDXSetResourceShareHandleNV");
    wglDXRegisterObjectNV =
        (PFNWGLDXREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXRegisterObjectNV");
    wglDXUnregisterObjectNV =
        (PFNWGLDXUNREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXUnregisterObjectNV");
    wglDXLockObjectsNV = (PFNWGLDXLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXLockObjectsNV");
    wglDXUnlockObjectsNV = (PFNWGLDXUNLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXUnlockObjectsNV");

    const bool success = wglDXOpenDeviceNV && wglDXCloseDeviceNV &&
                   wglDXSetResourceShareHandleNV &&
                   wglDXRegisterObjectNV && wglDXUnregisterObjectNV && wglDXLockObjectsNV &&
                   wglDXUnlockObjectsNV;

    if (!success) {
        qDebug() << "Failed to load WGL function pointers:";
        qDebug() << "wglDXOpenDeviceNV:" << (wglDXOpenDeviceNV ? "OK" : "FAIL");
        qDebug() << "wglDXCloseDeviceNV:" << (wglDXCloseDeviceNV ? "OK" : "FAIL");
        qDebug() << "wglDXSetResourceShareHandleNV:"
                 << (wglDXSetResourceShareHandleNV ? "OK" : "FAIL");
        qDebug() << "wglDXRegisterObjectNV:" << (wglDXRegisterObjectNV ? "OK" : "FAIL");
        qDebug() << "wglDXUnregisterObjectNV:" << (wglDXUnregisterObjectNV ? "OK" : "FAIL");
        qDebug() << "wglDXLockObjectsNV:" << (wglDXLockObjectsNV ? "OK" : "FAIL");
        qDebug() << "wglDXUnlockObjectsNV:" << (wglDXUnlockObjectsNV ? "OK" : "FAIL");
    }

    return success;
}

bool Nv12Render_Dxva2::createRgbRenderTarget()
{
    // 创建OpenGL纹理
    glGenTextures(1, &sharedTexture_);
    glBindTexture(GL_TEXTURE_2D, sharedTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

bool Nv12Render_Dxva2::convertNv12ToRgbStretchRect(LPDIRECT3DSURFACE9 nv12Surface)
{
    if (!nv12Surface || !rgbRenderTarget_) {
        qWarning() << "[Nv12Render_Dxva2] Missing surfaces for StretchRect conversion!";
        return false;
    }

    // 锁定WGL对象
    if (!wglDXLockObjectsNV(wglD3DDevice_, 1, &wglTextureHandle_)) {
        qWarning() << "[Nv12Render_Dxva2] Failed to lock WGL objects!";
        return false;
    }

    // 使用StretchRect进行格式转换和拷贝
    // 注意：这个方法依赖于D3D9驱动程序的内部转换能力
    // 某些驱动程序可能不支持从NV12直接转换到RGB
    const HRESULT hr = d3d9Device_->StretchRect(nv12Surface, nullptr, rgbRenderTarget_.Get(), nullptr,
                                          D3DTEXF_LINEAR);

    if (!wglDXUnlockObjectsNV(wglD3DDevice_, 1, &wglTextureHandle_)) {
        qWarning() << "[Nv12Render_Dxva2] Failed to unlock WGL objects!";
    }

    if (FAILED(hr)) {
        qWarning() << "[Nv12Render_Dxva2] StretchRect conversion failed, HRESULT:" << hr;
        return false;
    }

    return true;
}

bool Nv12Render_Dxva2::registerTextureWithOpenGL(int width, int height)
{
    if (!d3d9Device_) {
        qWarning() << "[Nv12Render_Dxva2] D3D9 device is null";
        return false;
    }

    // 清理之前的资源
    if (rgbRenderTarget_) {
        rgbRenderTarget_.Reset();
        sharedHandle_ = nullptr;
    }

    // 创建输出纹理
    const HRESULT hr = d3d9Device_->CreateRenderTarget(width, height,
                                                       D3DFMT_X8R8G8B8,     // OpenGL 兼容的格式
                                                       D3DMULTISAMPLE_NONE, // 无多重采样
                                                       0,                   // 多重采样质量
                                                       FALSE,               // 不可锁定
                                                       &rgbRenderTarget_,   // 输出纹理
                                                       &sharedHandle_       // 获取共享句柄
    );

    if (FAILED(hr)) {
        qWarning() << "[Nv12Render_Dxva2] Failed to create RGB render target, HRESULT:" << hr;
        return false;
    }

    if (!sharedHandle_) {
        qWarning() << "[Nv12Render_Dxva2] Shared handle is null!";
        return false;
    }

    if (!wglD3DDevice_ || !rgbRenderTarget_) {
        qWarning() << "[Nv12Render_Dxva2] Missing resources for OpenGL registration!";
        return false;
    }

    // 设置共享句柄
    if (sharedHandle_ && !wglDXSetResourceShareHandleNV(rgbRenderTarget_.Get(), sharedHandle_)) {
        DWORD error = GetLastError();
        qWarning() << "[Nv12Render_Dxva2] Failed setting Direct3D/OpenGL share handle for surface, error:" << error;

        return false;
    }

    // 注册RGB渲染目标表面
    wglTextureHandle_ =
        wglDXRegisterObjectNV(wglD3DDevice_,
                              rgbRenderTarget_.Get(),
                              sharedTexture_, GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
    if (!wglTextureHandle_) {
        DWORD error = GetLastError();
        qWarning() << "[Nv12Render_Dxva2] Failed to register texture, error:" << error;
        
        return false;
    }

    return true;
}

bool Nv12Render_Dxva2::drawFrame(GLuint id)
{
    if (!sharedTexture_ || !program_.isLinked() || !wglTextureHandle_) {
        qWarning() << "[Nv12Render_Dxva2] Not ready for drawing!";
        return false;
    }

    // 锁定WGL对象
    if (!wglDXLockObjectsNV(wglD3DDevice_, 1, &wglTextureHandle_)) {
        return false;
    }

    clearGL();

    // 使用着色器程序
    program_.bind();

    // 绑定纹理
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sharedTexture_);
    program_.setUniformValue("texture0", 0);

    // 绑定顶点缓冲区
    vbo_.bind();
    program_.enableAttributeArray("vertexIn");
    program_.enableAttributeArray("textureIn");
    program_.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 2, 0);
    program_.setAttributeBuffer("textureIn", GL_FLOAT, 2 * 4 * sizeof(GLfloat), 2, 0);

    // 绘制
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // 清理
    program_.disableAttributeArray("vertexIn");
    program_.disableAttributeArray("textureIn");
    vbo_.release();
    glBindTexture(GL_TEXTURE_2D, 0);
    program_.release();

    // 解锁WGL对象
    if (!wglDXUnlockObjectsNV(wglD3DDevice_, 1, &wglTextureHandle_)) {
        return false;
    }

    return true;
}

#endif