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
        uniform sampler2D texture0;
        varying vec2 textureOut;

        void main(void)
        {
            gl_FragColor = texture2D(texture0, textureOut);
        }
    )";
} // namespace

Nv12Render_Dxva2::Nv12Render_Dxva2(IDirect3DDevice9Ex *d3d9Device)
    : d3d9Device_(d3d9Device ? d3d9Device : DXVA2Utils::getDXVA2Device())
{
    qDebug() << "DXVA2 Constructor called";

    if (!d3d9Device_) {
        qDebug() << "No D3D9 device provided, creating own";
        initializeD3D9();
        ownD3DDevice_ = true;
    }
}

Nv12Render_Dxva2::~Nv12Render_Dxva2()
{
    qDebug() << "DXVA2 Destructor called";

    cleanup();

    // 清理WGL设备
    if (wglD3DDevice_) {
        qDebug() << "Closing WGL D3D device";
        wglDXCloseDeviceNV(wglD3DDevice_);
        wglD3DDevice_ = nullptr;
    }

    vbo_.destroy();

    if (ownD3DDevice_ && d3d9Device_) {
        d3d9Device_.Reset();
    }
}

void Nv12Render_Dxva2::initialize(const int width, const int height, const bool horizontal,
                                  const bool vertical)
{
    qDebug() << "DXVA2 Initialize called with size:" << width << "x" << height;

    initializeOpenGLFunctions();

    videoWidth_ = width;
    videoHeight_ = height;
    flipHorizontal_ = horizontal;
    flipVertical_ = vertical;

    if (!validateOpenGLContext()) {
        qDebug() << "Invalid OpenGL context";
        return;
    }

    if (!initializeWGLInterop()) {
        qDebug() << "Failed to initialize WGL interop";
        return;
    }

    // 编译着色器
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vsrc);
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fsrc);
    program_.link();

    // 设置顶点数据
    GLfloat points[] = {
        // 位置坐标
        -1.0f,
        1.0f,
        1.0f,
        1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        // 纹理坐标
        flipHorizontal_ ? 1.0f : 0.0f,
        flipVertical_ ? 1.0f : 0.0f,
        flipHorizontal_ ? 0.0f : 1.0f,
        flipVertical_ ? 1.0f : 0.0f,
        flipHorizontal_ ? 1.0f : 0.0f,
        flipVertical_ ? 0.0f : 1.0f,
        flipHorizontal_ ? 0.0f : 1.0f,
        flipVertical_ ? 0.0f : 1.0f,
    };

    vbo_.create();
    vbo_.bind();
    vbo_.allocate(points, sizeof(points));

    clearGL();
    qDebug() << "DXVA2 Initialize completed successfully";
}

void Nv12Render_Dxva2::render(const decoder_sdk::Frame &frame)
{
    if (!frame.isValid() || frame.pixelFormat() != decoder_sdk::ImageFormat::kDxva2) {
        qDebug() << "Invalid frame or wrong pixel format";
        clearGL();
        return;
    }

    // 从Frame中提取DXVA2表面指针
    LPDIRECT3DSURFACE9 sourceSurface = reinterpret_cast<LPDIRECT3DSURFACE9>(frame.data(3));
    if (!sourceSurface) {
        qDebug() << "Invalid DXVA2 surface";
        return;
    }

    D3DSURFACE_DESC surfaceDesc;
    HRESULT hr = sourceSurface->GetDesc(&surfaceDesc);
    if (FAILED(hr)) {
        qDebug() << "Failed to get surface description";
        return;
    }

    // 检查是否需要重新创建RGB渲染目标
    if (!rgbRenderTarget_ || currentWidth_ != surfaceDesc.Width ||
        currentHeight_ != surfaceDesc.Height) {
        qDebug() << "Creating new RGB render target";

        cleanup();

        currentWidth_ = surfaceDesc.Width;
        currentHeight_ = surfaceDesc.Height;

        if (!createRgbRenderTarget(currentWidth_, currentHeight_)) {
            qDebug() << "Failed to create RGB render target";
            return;
        }

        if (!registerTextureWithOpenGL()) {
            qDebug() << "Failed to register texture with OpenGL";
            return;
        }
    }

    // 使用StretchRect转换NV12到RGB
    if (!convertNv12ToRgbStretchRect(sourceSurface)) {
        qDebug() << "Failed to convert NV12 to RGB";
        return;
    }
}

void Nv12Render_Dxva2::draw()
{
    if (!sharedTexture_ || !program_.isLinked() || !wglTextureHandle_) {
        qDebug() << "Not ready for drawing";
        clearGL();
        return;
    }

    // 锁定WGL对象
    if (!wglDXLockObjectsNV(wglD3DDevice_, 1, &wglTextureHandle_)) {
        qDebug() << "Failed to lock WGL objects";
        clearGL();
        return;
    }

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
    program_.setAttributeBuffer("textureIn", GL_FLOAT, 8 * sizeof(GLfloat), 2, 0);

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
        qDebug() << "Failed to unlock WGL objects";
    }
}

bool Nv12Render_Dxva2::initializeD3D9()
{
    qDebug() << "Initializing D3D9";

    ComPtr<IDirect3D9Ex> d3d9Ex;
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9Ex);
    if (FAILED(hr)) {
        qDebug() << "Failed to create D3D9Ex";
        return false;
    }

    // 参考mpv的设备创建参数
    D3DPRESENT_PARAMETERS presentParams = {};
    presentParams.BackBufferWidth = 1;
    presentParams.BackBufferHeight = 1;
    presentParams.BackBufferFormat = D3DFMT_UNKNOWN; // 改为UNKNOWN
    presentParams.BackBufferCount = 1;
    presentParams.SwapEffect = D3DSWAPEFFECT_DISCARD;
    presentParams.hDeviceWindow = NULL; // 改为NULL
    presentParams.Windowed = TRUE;
    presentParams.Flags = D3DPRESENTFLAG_VIDEO;
    presentParams.FullScreen_RefreshRateInHz = 0;
    presentParams.PresentationInterval = 0;

    // 使用mpv相同的创建标志
    hr = d3d9Ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                NULL, // 改为NULL
                                D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED |
                                    D3DCREATE_FPU_PRESERVE |
                                    D3DCREATE_DISABLE_DRIVER_MANAGEMENT, // 添加这个标志
                                &presentParams, nullptr, &d3d9Device_);

    if (FAILED(hr)) {
        qDebug() << "Failed to create D3D9 device, HRESULT:" << QString::number(hr, 16);
        return false;
    }

    qDebug() << "D3D9 device created successfully";
    return true;
}

bool Nv12Render_Dxva2::initializeWGLInterop()
{
    qDebug() << "Initializing WGL interop";

    if (!loadWGLExtensions()) {
        qDebug() << "Failed to load WGL extensions";
        return false;
    }

    if (!d3d9Device_) {
        qDebug() << "D3D9 device is null";
        return false;
    }

    HRESULT hr = d3d9Device_->TestCooperativeLevel();
    if (FAILED(hr)) {
        qDebug() << "D3D9 device is in error state, HRESULT:" << hr;
        return false;
    }

    wglD3DDevice_ = wglDXOpenDeviceNV(d3d9Device_.Get());
    if (!wglD3DDevice_) {
        DWORD error = GetLastError();
        qDebug() << "Failed to open D3D device for WGL interop, error:" << error;
        return false;
    }

    qDebug() << "WGL interop initialized successfully";
    return true;
}

void Nv12Render_Dxva2::cleanup()
{
    qDebug() << "Cleaning up DXVA2 resources";

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

    currentWidth_ = 0;
    currentHeight_ = 0;
}

bool Nv12Render_Dxva2::loadWGLExtensions()
{
    // 检查WGL_NV_DX_interop扩展
    const char *extensions = nullptr;

    // 尝试获取WGL扩展字符串
    typedef const char *(WINAPI * PFNWGLGETEXTENSIONSSTRINGARBPROC)(HDC hdc);
    PFNWGLGETEXTENSIONSSTRINGARBPROC wglGetExtensionsStringARB =
        (PFNWGLGETEXTENSIONSSTRINGARBPROC)wglGetProcAddress("wglGetExtensionsStringARB");

    if (wglGetExtensionsStringARB) {
        HDC hdc = wglGetCurrentDC();
        if (hdc) {
            extensions = wglGetExtensionsStringARB(hdc);
        }
    }

    if (!extensions || !strstr(extensions, "WGL_NV_DX_interop")) {
        qDebug() << "WGL_NV_DX_interop extension not supported";
        qDebug() << "Available extensions:" << (extensions ? extensions : "none");
        return false;
    }

    qDebug() << "WGL_NV_DX_interop extension found";

    // 加载函数指针
    wglDXOpenDeviceNV = (PFNWGLDXOPENDEVICENVPROC)wglGetProcAddress("wglDXOpenDeviceNV");
    wglDXCloseDeviceNV = (PFNWGLDXCLOSEDEVICENVPROC)wglGetProcAddress("wglDXCloseDeviceNV");
    wglDXSetResourceShareHandleNV = (PFNWGLDXSETRESOURCESHAREHANDLENVPROC)wglGetProcAddress(
        "wglDXSetResourceShareHandleNV"); // 添加这行
    wglDXRegisterObjectNV =
        (PFNWGLDXREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXRegisterObjectNV");
    wglDXUnregisterObjectNV =
        (PFNWGLDXUNREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXUnregisterObjectNV");
    wglDXLockObjectsNV = (PFNWGLDXLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXLockObjectsNV");
    wglDXUnlockObjectsNV = (PFNWGLDXUNLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXUnlockObjectsNV");

    bool success = wglDXOpenDeviceNV && wglDXCloseDeviceNV &&
                   wglDXSetResourceShareHandleNV && // 更新检查条件
                   wglDXRegisterObjectNV && wglDXUnregisterObjectNV && wglDXLockObjectsNV &&
                   wglDXUnlockObjectsNV;

    if (!success) {
        qDebug() << "Failed to load WGL function pointers:";
        qDebug() << "wglDXOpenDeviceNV:" << (wglDXOpenDeviceNV ? "OK" : "FAIL");
        qDebug() << "wglDXCloseDeviceNV:" << (wglDXCloseDeviceNV ? "OK" : "FAIL");
        qDebug() << "wglDXSetResourceShareHandleNV:"
                 << (wglDXSetResourceShareHandleNV ? "OK" : "FAIL"); // 添加检查
        qDebug() << "wglDXRegisterObjectNV:" << (wglDXRegisterObjectNV ? "OK" : "FAIL");
        qDebug() << "wglDXUnregisterObjectNV:" << (wglDXUnregisterObjectNV ? "OK" : "FAIL");
        qDebug() << "wglDXLockObjectsNV:" << (wglDXLockObjectsNV ? "OK" : "FAIL");
        qDebug() << "wglDXUnlockObjectsNV:" << (wglDXUnlockObjectsNV ? "OK" : "FAIL");
    }

    return success;
}

bool Nv12Render_Dxva2::createRgbRenderTarget(int width, int height)
{
    if (!d3d9Device_) {
        qDebug() << "D3D9 device is null";
        return false;
    }

    // 清理之前的资源
    if (rgbRenderTarget_) {
        rgbRenderTarget_.Reset();
        sharedHandle_ = nullptr;
    }

    // 参考mpv的实现：直接创建渲染目标表面而不是纹理
    HRESULT hr = d3d9Device_->CreateRenderTarget(width, height,
                                                 D3DFMT_X8R8G8B8,     // 使用mpv相同的格式
                                                 D3DMULTISAMPLE_NONE, // 无多重采样
                                                 0,                   // 多重采样质量
                                                 FALSE,               // 不可锁定
                                                 &rgbRenderTarget_,
                                                 &sharedHandle_ // 获取共享句柄
    );

    if (FAILED(hr)) {
        qDebug() << "Failed to create RGB render target, HRESULT:" << QString::number(hr, 16);
        return false;
    }

    if (!sharedHandle_) {
        qDebug() << "Warning: Shared handle is null";
    }

    qDebug() << "Created RGB render target successfully, handle:" << sharedHandle_;
    return true;
}

bool Nv12Render_Dxva2::convertNv12ToRgbStretchRect(LPDIRECT3DSURFACE9 nv12Surface)
{
    if (!nv12Surface || !rgbRenderTarget_) {
        qDebug() << "Missing surfaces for StretchRect conversion";
        return false;
    }

    // 锁定WGL对象
    if (!wglDXLockObjectsNV(wglD3DDevice_, 1, &wglTextureHandle_)) {
        qDebug() << "Failed to lock WGL objects";
        return false;
    }

    // 使用StretchRect进行格式转换和拷贝
    // 注意：这个方法依赖于D3D9驱动程序的内部转换能力
    // 某些驱动程序可能不支持从NV12直接转换到RGB
    HRESULT hr = d3d9Device_->StretchRect(nv12Surface, nullptr, rgbRenderTarget_.Get(), nullptr,
                                          D3DTEXF_LINEAR);

    if (FAILED(hr)) {
        qDebug() << "StretchRect conversion failed, HRESULT:" << QString::number(hr, 16);

        // 如果StretchRect失败，尝试使用软件转换
        return false;
    }

    if (!wglDXUnlockObjectsNV(wglD3DDevice_, 1, &wglTextureHandle_)) {
        qDebug() << "Failed to unlock WGL objects";
    }

    qDebug() << "StretchRect conversion successful";
    return true;
}

bool Nv12Render_Dxva2::registerTextureWithOpenGL()
{
    if (!wglD3DDevice_ || !rgbRenderTarget_) {
        qDebug() << "Missing resources for OpenGL registration";
        qDebug() << "wglD3DDevice_:" << (wglD3DDevice_ ? "valid" : "null");
        qDebug() << "rgbRenderTarget_:" << (rgbRenderTarget_ ? "valid" : "null");
        return false;
    }

    // 确保OpenGL上下文是当前的
    if (!wglGetCurrentContext()) {
        qDebug() << "No current OpenGL context";
        return false;
    }

    // 清理之前的纹理
    if (sharedTexture_ != 0) {
        glDeleteTextures(1, &sharedTexture_);
        sharedTexture_ = 0;
    }

    // 生成OpenGL纹理
    glGenTextures(1, &sharedTexture_);
    if (sharedTexture_ == 0) {
        qDebug() << "Failed to generate OpenGL texture";
        return false;
    }

    // 绑定纹理并设置参数
    glBindTexture(GL_TEXTURE_2D, sharedTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // 关键步骤：设置共享句柄（参考mpv实现）
    if (sharedHandle_ && !wglDXSetResourceShareHandleNV(rgbRenderTarget_.Get(), sharedHandle_)) {
        DWORD error = GetLastError();
        qDebug() << "Failed setting Direct3D/OpenGL share handle for surface, error:"
                 << QString::number(error, 16);

        // 清理失败的纹理
        glDeleteTextures(1, &sharedTexture_);
        sharedTexture_ = 0;
        return false;
    }

    // 注册RGB渲染目标表面
    wglTextureHandle_ =
        wglDXRegisterObjectNV(wglD3DDevice_,
                              rgbRenderTarget_.Get(), // 直接使用渲染目标表面
                              sharedTexture_, GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);

    if (!wglTextureHandle_) {
        DWORD error = GetLastError();
        qDebug() << "Failed to register RGB surface with WGL, error:" << QString::number(error, 16);
        qDebug() << "Trying read-write access...";

        // 尝试读写访问模式
        wglTextureHandle_ =
            wglDXRegisterObjectNV(wglD3DDevice_, rgbRenderTarget_.Get(), sharedTexture_,
                                  GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);

        if (!wglTextureHandle_) {
            error = GetLastError();
            qDebug() << "Failed to register with read-write access, error:"
                     << QString::number(error, 16);

            // 清理失败的纹理
            glDeleteTextures(1, &sharedTexture_);
            sharedTexture_ = 0;
            return false;
        }
    }

    qDebug() << "Successfully registered RGB surface with OpenGL";
    return true;
}

bool Nv12Render_Dxva2::validateOpenGLContext()
{
    GLint major, minor;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);

    if (major == 0 && minor == 0) {
        const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
        qDebug() << "OpenGL version string:" << (version ? version : "unknown");
        return version != nullptr;
    }

    qDebug() << "OpenGL version:" << major << "." << minor;
    return true;
}

void Nv12Render_Dxva2::clearGL()
{
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

#endif