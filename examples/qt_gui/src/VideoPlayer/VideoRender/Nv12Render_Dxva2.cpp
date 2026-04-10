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
bool unregisterWglTexture(wgl::WglDeviceRef &device, HANDLE &handle)
{
    if (!handle) {
        return true;
    }
    if (!device.isValid()) {
        return false;
    }

    HANDLE interopHandle = handle;
    device.wglDXUnlockObjectsNV(1, &interopHandle);
    const bool ok = device.wglDXUnregisterObjectNV(interopHandle);
    if (ok) {
        handle = nullptr;
    }
    return ok;
}
} // namespace

Nv12Render_Dxva2::Nv12Render_Dxva2() : VideoRender()
{
    initializeD3DResource(dxva2_utils::getDXVA2Device(), dxva2_utils::getWglDeviceRef());
}

Nv12Render_Dxva2::~Nv12Render_Dxva2()
{
    cleanup();
    d3d9Device_.Reset();
}

bool Nv12Render_Dxva2::shouldRebuild() const
{
    return shouldReBuild_;
}

QString Nv12Render_Dxva2::renderName() const
{
    return QStringLiteral("DXVA2 Interop To OpenGL Render");
}

bool Nv12Render_Dxva2::initRenderVbo(const bool horizontal, const bool vertical)
{
    horizontalMirror_ = horizontal;
    verticalMirror_ = vertical;
    return true;
}

bool Nv12Render_Dxva2::initRenderShader(const decoder_sdk::Frame &frame)
{
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
#ifdef AMF_AVAILABLE
    else if (curPixelForamt == decoder_sdk::ImageFormat::kAmf) {
        auto *const amfSurface = reinterpret_cast<amf::AMFSurface *>(frame.data(0));
        if (amfSurface) {
            sourceSurface = amf_utils::getPackedSurfaceDX9(amfSurface);
        }
    }
#endif

    if (!sourceSurface) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Missing required resources!");
        cleanup();
        return false;
    }

    ComPtr<IDirect3DDevice9> textureDevice;
    sourceSurface->GetDevice(&textureDevice);
    ComPtr<IDirect3DDevice9Ex> textureDeviceEx;
    HRESULT hr =
        textureDevice->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void **)&textureDeviceEx);
    if (FAILED(hr)) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Unsupported device!");
        cleanup();
        return false;
    }

    if (textureDeviceEx.Get() != d3d9Device_.Get()) {
        qInfo() << QStringLiteral(
            "[Nv12Render_Dxva2] The decoding-side D3D9 Device is inconsistent with the one "
            "currently in use; therefore, the D3D9 Resource needs to be reinitialized.");
        if (!initializeD3DResource(textureDeviceEx)) {
            qWarning() << QStringLiteral("[Nv12Render_Dxva2] Reinitialize D3D9 resource failed!");
            cleanup();
            return false;
        }
    }

    if (!checkWGLInterop()) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Failed to initialize WGL interop!");
        cleanup();
        return false;
    }

    if (!registerTextureWithOpenGL(frame.width(), frame.height())) {
        qWarning() << QStringLiteral(
            "[Nv12Render_Dxva2] Failed to register D3D texture to OpenGL texture!");
        cleanup();
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
#ifdef AMF_AVAILABLE
    else if (curPixelForamt == decoder_sdk::ImageFormat::kAmf) {
        auto *const amfSurface = reinterpret_cast<amf::AMFSurface *>(frame.data(0));
        if (amfSurface) {
            sourceSurface = amf_utils::getPackedSurfaceDX9(amfSurface);
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

    return blitToCurrentFbo(frame.width(), frame.height());
}

void Nv12Render_Dxva2::cleanupAllResources()
{
    cleanup();
}

bool Nv12Render_Dxva2::initializeD3DResource(const ComPtr<IDirect3DDevice9Ex> &d3d9Device,
                                             const wgl::WglDeviceRef &wglDevice)
{
    if (!d3d9Device.Get()) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Can not get D3D9 Device!");
        return false;
    }

    wgl::WglDeviceRef newWglDevice;
    if (wglDevice.isValid()) {
        newWglDevice = wglDevice;
    } else {
        newWglDevice = wgl::WglDeviceRef(d3d9Device.Get());
    }

    if (!newWglDevice.isValid()) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Can not get wgl Device!");
        return false;
    }

    if (wglTextureHandle_ && !unregisterWglTexture(wglD3DDevice_, wglTextureHandle_)) {
        qWarning() << QStringLiteral(
            "[Nv12Render_Dxva2] Failed to unregister WGL object before switching device!");
        return false;
    }

    rgbRenderTarget_.Reset();
    sharedHandle_ = nullptr;

    d3d9Device_ = d3d9Device;
    wglD3DDevice_ = newWglDevice;

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
    if (wglTextureHandle_ && !unregisterWglTexture(wglD3DDevice_, wglTextureHandle_) &&
        wglD3DDevice_.isValid()) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Failed to unregister WGL object!");
    }

    if (sharedTexture_) {
        glDeleteTextures(1, &sharedTexture_);
        sharedTexture_ = 0;
    }

    if (glInteropReadFbo_) {
        glDeleteFramebuffers(1, &glInteropReadFbo_);
        glInteropReadFbo_ = 0;
    }

    if (rgbRenderTarget_) {
        rgbRenderTarget_.Reset();
    }
    sharedHandle_ = nullptr;
}

bool Nv12Render_Dxva2::createRgbRenderTarget()
{
    if (sharedTexture_) {
        glDeleteTextures(1, &sharedTexture_);
        sharedTexture_ = 0;
    }

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

    if (wglTextureHandle_ && !unregisterWglTexture(wglD3DDevice_, wglTextureHandle_)) {
        qWarning() << QStringLiteral(
            "[Nv12Render_Dxva2] Failed to unregister previous WGL object!");
        return false;
    }

    rgbRenderTarget_.Reset();
    sharedHandle_ = nullptr;

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
        rgbRenderTarget_.Reset();
        sharedHandle_ = nullptr;
        return false;
    }

    if (!sharedHandle_) {
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Shared handle is null!");
        rgbRenderTarget_.Reset();
        return false;
    }

    if (!wglD3DDevice_.isValid() || !rgbRenderTarget_) {
        qWarning() << "[Nv12Render_Dxva2] Missing resources for OpenGL registration!";
        rgbRenderTarget_.Reset();
        sharedHandle_ = nullptr;
        return false;
    }

    if (sharedHandle_ &&
        !wgl::wglDXSetResourceShareHandleNV(rgbRenderTarget_.Get(), sharedHandle_)) {
        DWORD error = GetLastError();
        qWarning() << QStringLiteral(
                          "[Nv12Render_Dxva2] Failed setting Direct3D/OpenGL share handle for "
                          "surface, error:")
                   << error;

        rgbRenderTarget_.Reset();
        sharedHandle_ = nullptr;
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, sharedTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    wglTextureHandle_ = wglD3DDevice_.wglDXRegisterObjectNV(rgbRenderTarget_.Get(), sharedTexture_,
                                                            GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
    if (!wglTextureHandle_) {
        DWORD error = GetLastError();
        qWarning() << QStringLiteral("[Nv12Render_Dxva2] Failed to register texture, error:")
                   << error;

        rgbRenderTarget_.Reset();
        sharedHandle_ = nullptr;
        return false;
    }
    wglD3DDevice_.wglDXLockObjectsNV(1, &wglTextureHandle_);

    return true;
}

bool Nv12Render_Dxva2::ensureInteropReadFbo()
{
    if (!sharedTexture_) {
        return false;
    }

    if (!glInteropReadFbo_) {
        glGenFramebuffers(1, &glInteropReadFbo_);
        if (!glInteropReadFbo_) {
            return false;
        }
    }

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    glBindFramebuffer(GL_FRAMEBUFFER, glInteropReadFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sharedTexture_, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    return status == GL_FRAMEBUFFER_COMPLETE;
}

bool Nv12Render_Dxva2::blitToCurrentFbo(int width, int height)
{
    if (!wglTextureHandle_) {
        return false;
    }

    if (!ensureInteropReadFbo()) {
        return false;
    }

    GLint prevReadFbo = 0;
    GLint prevDrawFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, glInteropReadFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDrawFbo));

    const int srcX0 = horizontalMirror_ ? width : 0;
    const int srcX1 = horizontalMirror_ ? 0 : width;
    const int srcY0 = verticalMirror_ ? 0 : height;
    const int srcY1 = verticalMirror_ ? height : 0;

    glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, 0, 0, width, height, GL_COLOR_BUFFER_BIT,
                      GL_LINEAR);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevReadFbo));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDrawFbo));

    return true;
}

#endif
