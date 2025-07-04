#ifndef NV12RENDER_DXVA2_H
#define NV12RENDER_DXVA2_H
#ifdef DXVA2_AVAILABLE

#include "VideoRender.h"

#include <QDebug>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>

#ifdef _WIN32
#include <Windows.h>
#include <d3d9.h>
#include <dxva2api.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

// WGL-DX interop definitions
#define WGL_ACCESS_READ_ONLY_NV 0x00000000
#define WGL_ACCESS_READ_WRITE_NV 0x00000001
#define WGL_ACCESS_WRITE_DISCARD_NV 0x00000002

// 在现有的WGL函数指针定义中添加
typedef BOOL(WINAPI *PFNWGLDXSETRESOURCESHAREHANDLENVPROC)(void *dxObject, HANDLE shareHandle);
typedef HANDLE(WINAPI *PFNWGLDXOPENDEVICENVPROC)(void *dxDevice);
typedef BOOL(WINAPI *PFNWGLDXCLOSEDEVICENVPROC)(HANDLE hDevice);
typedef HANDLE(WINAPI *PFNWGLDXREGISTEROBJECTNVPROC)(HANDLE hDevice, void *dxObject, GLuint name,
                                                     GLenum type, GLenum access);
typedef BOOL(WINAPI *PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE hDevice, HANDLE hObject);
typedef BOOL(WINAPI *PFNWGLDXOBJECTACCESSNVPROC)(HANDLE hObject, GLenum access);
typedef BOOL(WINAPI *PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);
typedef BOOL(WINAPI *PFNWGLDXUNLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);

// 在WGL扩展定义部分添加
typedef const char *(WINAPI *PFNWGLGETEXTENSIONSSTRINGARBPROC)(HDC hdc);

#endif

class Nv12Render_Dxva2 : public VideoRender, public QOpenGLFunctions {
public:
    explicit Nv12Render_Dxva2(IDirect3DDevice9Ex *d3d9Device = nullptr);
    ~Nv12Render_Dxva2();

    void initialize(const int width, const int height, const bool horizontal = false,
                    const bool vertical = false) override;
    void render(const decoder_sdk::Frame &frame) override;
    void draw() override;

private:
    // D3D9 related
    ComPtr<IDirect3DDevice9Ex> d3d9Device_;
    bool ownD3DDevice_ = false;

    // RGB纹理和表面
    // 移除rgbTexture_成员，只保留rgbRenderTarget_
    // ComPtr<IDirect3DTexture9> rgbTexture_;  // 删除这行
    ComPtr<IDirect3DSurface9> rgbRenderTarget_;
    HANDLE sharedHandle_ = nullptr;

    // WGL extension function pointers
    PFNWGLDXSETRESOURCESHAREHANDLENVPROC wglDXSetResourceShareHandleNV = nullptr;
    PFNWGLDXOPENDEVICENVPROC wglDXOpenDeviceNV = nullptr;
    PFNWGLDXCLOSEDEVICENVPROC wglDXCloseDeviceNV = nullptr;
    PFNWGLDXREGISTEROBJECTNVPROC wglDXRegisterObjectNV = nullptr;
    PFNWGLDXUNREGISTEROBJECTNVPROC wglDXUnregisterObjectNV = nullptr;
    PFNWGLDXLOCKOBJECTSNVPROC wglDXLockObjectsNV = nullptr;
    PFNWGLDXUNLOCKOBJECTSNVPROC wglDXUnlockObjectsNV = nullptr;

    // WGL interop handles
    HANDLE wglD3DDevice_ = nullptr;
    HANDLE wglTextureHandle_ = nullptr;

    // OpenGL resources
    GLuint sharedTexture_ = 0;
    QOpenGLShaderProgram program_;
    QOpenGLBuffer vbo_;

    // Video properties
    int videoWidth_ = 0;
    int videoHeight_ = 0;
    int currentWidth_ = 0;
    int currentHeight_ = 0;
    bool flipHorizontal_ = false;
    bool flipVertical_ = false;

    // Private methods
    bool initializeD3D9();
    bool initializeWGLInterop();
    void cleanup();
    bool loadWGLExtensions();
    bool createRgbRenderTarget(int width, int height);
    bool convertNv12ToRgbStretchRect(LPDIRECT3DSURFACE9 nv12Surface);
    bool registerTextureWithOpenGL();
    bool validateOpenGLContext();
    void clearGL();
};

#endif
#endif // NV12RENDER_DXVA2_H