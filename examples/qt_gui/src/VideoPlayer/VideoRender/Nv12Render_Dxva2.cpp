#ifdef DXVA2_AVAILABLE
#include "Nv12Render_Dxva2.h"

#ifdef QSV_AVAILABLE
#include "mfxstructures.h"
#endif

#ifdef _WIN32
#include <Windows.h>
#include <d3d9.h>
#include <dxva2api.h>
#endif

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

Nv12Render_Dxva2::Nv12Render_Dxva2() : VideoRender()
{
    initializeD3DResource(dxva2_utils::getDXVA2Device(), dxva2_utils::getWglDeviceRef());
}

Nv12Render_Dxva2::~Nv12Render_Dxva2()
{
    cleanup();
    d3d9Device_.Reset();

    // 清理VBO
    vbo_.destroy();
}

bool Nv12Render_Dxva2::shouldRebuild() const
{
    return shouldReBuild_;
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
    const auto curPixelForamt = frame.pixelFormat();

    // 得到当前纹理中设备
    LPDIRECT3DSURFACE9 sourceSurface = nullptr;
    if (curPixelForamt == decoder_sdk::ImageFormat::kDxva2) {
        sourceSurface = reinterpret_cast<LPDIRECT3DSURFACE9>(frame.data(3));
    }
#ifdef QSV_AVAILABLE
    else if (curPixelForamt == decoder_sdk::ImageFormat::kQsv) {
        auto *const mfxSurface = reinterpret_cast<mfxFrameSurface1 *>(frame.data(3));
        if (mfxSurface) {
            sourceSurface = reinterpret_cast<LPDIRECT3DSURFACE9>(
                reinterpret_cast<mfxHDLPair *>(mfxSurface->Data.MemId)->first);
        }
    }
#endif

    if (!sourceSurface) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Missing required resources!");
        return false;
    }

    ComPtr<IDirect3DDevice9> textureDevice;
    sourceSurface->GetDevice(&textureDevice);
    ComPtr<IDirect3DDevice9Ex> textureDeviceEx;
    HRESULT hr =
        textureDevice->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void **)&textureDeviceEx);
    if (FAILED(hr)) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Unsupported device!");
        return false;
    }

    if (textureDeviceEx.Get() != d3d9Device_.Get()) {
        qInfo() << QStringLiteral(
            "[Nv12Render_Dxva2] The decoding-side D3D9 Device is inconsistent with the one "
            "currently in use; therefore, the D3D9 Resource needs to be reinitialized.");
        if (!initializeD3DResource(textureDeviceEx)) {
            qWarning() << QStringLiteral("[Nv12Render_Dxva2] Reinitialize D3D9 resource failed!");
            return false;
        }
    }

    if (!checkWGLInterop()) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Failed to initialize WGL interop!");
        return false;
    }

    if (!registerTextureWithOpenGL(frame.width(), frame.height())) {
        qWarning() << QStringLiteral(
            "[Nv12Render_Dxva2] Failed to register D3D texture to OpenGL texture!");
        return false;
    }

    return true;
}

bool Nv12Render_Dxva2::renderFrame(const decoder_sdk::Frame &frame)
{
    if (!frame.isValid()) {
        return false;
    }
    const auto curPixelForamt = frame.pixelFormat();

    // 从Frame中提取DXVA2表面指针
    LPDIRECT3DSURFACE9 sourceSurface = nullptr;
    if (curPixelForamt == decoder_sdk::ImageFormat::kDxva2) {
        sourceSurface = reinterpret_cast<LPDIRECT3DSURFACE9>(frame.data(3));
    }
#ifdef QSV_AVAILABLE
    else if (curPixelForamt == decoder_sdk::ImageFormat::kQsv) {
        auto *const mfxSurface = reinterpret_cast<mfxFrameSurface1 *>(frame.data(3));
        if (mfxSurface) {
            sourceSurface = reinterpret_cast<LPDIRECT3DSURFACE9>(
                reinterpret_cast<mfxHDLPair *>(mfxSurface->Data.MemId)->first);
        }
    }
#endif

    if (!sourceSurface) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Invalid DXVA2 surface!");
        return false;
    }

    // 使用StretchRect转换NV12到RGB
    if (!convertNv12ToRgbStretchRect(sourceSurface, frame)) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Failed to convert NV12 to RGB!");
        return false;
    }

    // 绘制
    return drawFrame(sharedTexture_);
}

bool Nv12Render_Dxva2::initializeD3DResource(const ComPtr<IDirect3DDevice9Ex> &d3d9Device,
                                             const wgl::WglDeviceRef &wglDevice)
{
    d3d9Device_ = d3d9Device;

    if (!d3d9Device_.Get()) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Can not get D3D9 Device!");
        return false;
    }

    // 如果传入的wgl设备有效，就使用传入的设备，否则使用d3d11 device新建
    if (wglDevice.isValid()) {
        wglD3DDevice_ = wglDevice;
    } else {
        wglD3DDevice_ = wgl::WglDeviceRef(d3d9Device_.Get());
    }

    if (!wglD3DDevice_.isValid()) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Can not get wgl Device!");
        return false;
    }

    return true;
}

bool Nv12Render_Dxva2::checkWGLInterop()
{
    if (!d3d9Device_) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] D3D9 device is null!");
        return false;
    }

    if (!wglD3DDevice_.isValid()) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] WGL device is invalid!");
        return false;
    }

    return true;
}

void Nv12Render_Dxva2::cleanup()
{
    if (wglTextureHandle_ && wglD3DDevice_.isValid()) {
        wglD3DDevice_.wglDXUnregisterObjectNV(wglTextureHandle_);
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

bool Nv12Render_Dxva2::convertNv12ToRgbStretchRect(LPDIRECT3DSURFACE9 nv12Surface,
                                                   const decoder_sdk::Frame &frame)
{
    if (!nv12Surface || !rgbRenderTarget_) {
        qWarning() << QStringLiteral(
            "[Nv12Render_Dxva2] Missing surfaces for StretchRect conversion!");
        return false;
    }

    // 检查设备是否相同
    ComPtr<IDirect3DDevice9> textureDevice;
    nv12Surface->GetDevice(&textureDevice);

    if (textureDevice.Get() != d3d9Device_.Get()) {
        // 不同设备，重建Render
        shouldReBuild_ = true;
        qWarning() << QStringLiteral(
            "[Nv12Render_Dxva2] The decoding-side D3D9 Device is inconsistent with the one "
            "currently in use; therefore, the render needs to be rebuild.");
        return false;
    }

    // 设置源矩形（实际视频内容区域）
    RECT sourceRect = {
        0, 0,
        static_cast<LONG>(frame.width()), // 使用实际视频宽度
        static_cast<LONG>(frame.height()) // 使用实际视频高度
    };

    // 设置目标矩形（输出纹理区域）
    RECT destRect = {0, 0, static_cast<LONG>(frame.width()), static_cast<LONG>(frame.height())};

    // 使用StretchRect进行格式转换和拷贝
    // 注意：这个方法依赖于D3D9驱动程序的内部转换能力
    // 某些驱动程序可能不支持从NV12直接转换到RGB
    wglD3DDevice_.wglDXUnlockObjectsNV(1, &wglTextureHandle_);
    const HRESULT hr = d3d9Device_->StretchRect(nv12Surface, &sourceRect, rgbRenderTarget_.Get(),
                                                &destRect, D3DTEXF_LINEAR);
    wglD3DDevice_.wglDXLockObjectsNV(1, &wglTextureHandle_);

    if (FAILED(hr)) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] StretchRect conversion failed, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    return true;
}

bool Nv12Render_Dxva2::registerTextureWithOpenGL(int width, int height)
{
    if (!d3d9Device_) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] D3D9 device is null");
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
        qWarning() << QStringLiteral(
                          "[Nv12Render_Dxva2] Failed to create RGB render target, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    if (!sharedHandle_) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Shared handle is null!");
        return false;
    }

    if (!wglD3DDevice_.isValid() || !rgbRenderTarget_) {
        qWarning() << "[Nv12Render_Dxva2] Missing resources for OpenGL registration!";
        return false;
    }

    // 设置共享句柄
    if (sharedHandle_ &&
        !wgl::wglDXSetResourceShareHandleNV(rgbRenderTarget_.Get(), sharedHandle_)) {
        DWORD error = GetLastError();
        qWarning() << QStringLiteral(
                          "[Nv12Render_Dxva2] Failed setting Direct3D/OpenGL share handle for "
                          "surface, error:")
                   << error;

        return false;
    }

    // 注册RGB渲染目标表面
    wglTextureHandle_ = wglD3DDevice_.wglDXRegisterObjectNV(rgbRenderTarget_.Get(), sharedTexture_,
                                                            GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
    if (!wglTextureHandle_) {
        DWORD error = GetLastError();
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Failed to register texture, error:")
                   << error;

        return false;
    }
    wglD3DDevice_.wglDXLockObjectsNV(1, &wglTextureHandle_);

    return true;
}

bool Nv12Render_Dxva2::drawFrame(GLuint id)
{
    if (!sharedTexture_ || !program_.isLinked() || !wglTextureHandle_) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Not ready for drawing!");
        return false;
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
    program_.setAttributeBuffer("textureIn", GL_FLOAT, 2 * 4 * sizeof(GLfloat), 2, 0);

    // 绘制
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // 清理
    program_.disableAttributeArray("vertexIn");
    program_.disableAttributeArray("textureIn");
    vbo_.release();
    glBindTexture(GL_TEXTURE_2D, 0);
    program_.release();

    return true;
}

#endif