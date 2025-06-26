#pragma once
#include "VideoRender.h"

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QMutex>

#ifdef _WIN32
#include <Windows.h>
#include <d3d9.h>
#include <dxva2api.h>
#include <GL/gl.h>

// 手动定义 WGL 扩展，避免依赖 wglext.h
#ifndef WGL_NV_DX_interop
#define WGL_ACCESS_READ_ONLY_NV 0x00000000
#define WGL_ACCESS_READ_WRITE_NV 0x00000001
#define WGL_ACCESS_WRITE_DISCARD_NV 0x00000002

typedef BOOL(WINAPI *PFNWGLDXSETRESOURCESHAREHANDLENVPROC)(void *dxObject, HANDLE shareHandle);
typedef HANDLE(WINAPI *PFNWGLDXOPENDEVICENVPROC)(void *dxDevice);
typedef BOOL(WINAPI *PFNWGLDXCLOSEDEVICENVPROC)(HANDLE hDevice);
typedef HANDLE(WINAPI *PFNWGLDXREGISTEROBJECTNVPROC)(HANDLE hDevice, void *dxObject, GLuint name, GLenum type, GLenum access);
typedef BOOL(WINAPI *PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE hDevice, HANDLE hObject);
typedef BOOL(WINAPI *PFNWGLDXOBJECTACCESSNVPROC)(HANDLE hObject, GLenum access);
typedef BOOL(WINAPI *PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);
typedef BOOL(WINAPI *PFNWGLDXUNLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);
#endif

#endif

class Nv12Render_Dxva2 : public QOpenGLFunctions, public VideoRender
{
public:
    Nv12Render_Dxva2(IDirect3DDevice9 *d3dDevice = nullptr);
    ~Nv12Render_Dxva2() override;

public:
    void initialize(const int width, const int height, const bool horizontal = false, const bool vertical = false) override;
    void render(const decoder_sdk::Frame &frame) override;
    void draw() override;
    QOpenGLFramebufferObject *getFrameBuffer(const QSize &size) override;

private:
    void clearGL();
    bool initializeD3D9();
    bool initializeWGL();
    void cleanupD3D9();
    void cleanupWGL();

private:
    // 纹理同步锁
    QMutex mtx_;

    // D3D9 相关对象
    IDirect3D9 *d3d9_ = nullptr;
    IDirect3DDevice9 *d3dDevice_ = nullptr;
    
    // 双缓冲D3D表面
    IDirect3DSurface9 *d3dSurfaceCurrentY_ = nullptr;
    IDirect3DSurface9 *d3dSurfaceCurrentUV_ = nullptr;
    IDirect3DSurface9 *d3dSurfaceNextY_ = nullptr;
    IDirect3DSurface9 *d3dSurfaceNextUV_ = nullptr;

    // WGL 互操作相关 - 双缓冲
    HANDLE wglD3DDevice_ = nullptr;
    HANDLE wglD3DSurfaceCurrentY_ = nullptr;
    HANDLE wglD3DSurfaceCurrentUV_ = nullptr;
    HANDLE wglD3DSurfaceNextY_ = nullptr;
    HANDLE wglD3DSurfaceNextUV_ = nullptr;

    // OpenGL 纹理 - 双缓冲
    GLuint textureCurrentY_ = 0;
    GLuint textureCurrentUV_ = 0;
    GLuint textureNextY_ = 0;
    GLuint textureNextUV_ = 0;

    // OpenGL 相关对象
    QOpenGLShaderProgram program_;
    QOpenGLBuffer vbo_;

    // 视频尺寸
    int videoWidth_ = 0;
    int videoHeight_ = 0;

    // 是否需要释放D3D设备
    bool ownD3DDevice_ = false;
    
    // 资源注册状态
    bool resourceCurrentYRegisteredFailed_ = false;
    bool resourceCurrentUVRegisteredFailed_ = false;
    bool resourceNextYRegisteredFailed_ = false;
    bool resourceNextUVRegisteredFailed_ = false;

    // WGL 扩展函数指针
    PFNWGLDXOPENDEVICENVPROC wglDXOpenDeviceNV = nullptr;
    PFNWGLDXCLOSEDEVICENVPROC wglDXCloseDeviceNV = nullptr;
    PFNWGLDXREGISTEROBJECTNVPROC wglDXRegisterObjectNV = nullptr;
    PFNWGLDXUNREGISTEROBJECTNVPROC wglDXUnregisterObjectNV = nullptr;
    PFNWGLDXLOCKOBJECTSNVPROC wglDXLockObjectsNV = nullptr;
    PFNWGLDXUNLOCKOBJECTSNVPROC wglDXUnlockObjectsNV = nullptr;
};