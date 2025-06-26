#pragma once
#include "VideoRender.h"

#include <QMutex>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>

#ifdef _WIN32
#include <GL/gl.h>
#include <Windows.h>
#include <d3d11.h>

// 手动定义 WGL 扩展，避免依赖 wglext.h
#ifndef WGL_NV_DX_interop
#define WGL_ACCESS_READ_ONLY_NV 0x00000000
#define WGL_ACCESS_READ_WRITE_NV 0x00000001
#define WGL_ACCESS_WRITE_DISCARD_NV 0x00000002

typedef BOOL(WINAPI *PFNWGLDXSETRESOURCESHAREHANDLENVPROC)(void *dxObject, HANDLE shareHandle);
typedef HANDLE(WINAPI *PFNWGLDXOPENDEVICENVPROC)(void *dxDevice);
typedef BOOL(WINAPI *PFNWGLDXCLOSEDEVICENVPROC)(HANDLE hDevice);
typedef HANDLE(WINAPI *PFNWGLDXREGISTEROBJECTNVPROC)(HANDLE hDevice, void *dxObject, GLuint name,
                                                     GLenum type, GLenum access);
typedef BOOL(WINAPI *PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE hDevice, HANDLE hObject);
typedef BOOL(WINAPI *PFNWGLDXOBJECTACCESSNVPROC)(HANDLE hObject, GLenum access);
typedef BOOL(WINAPI *PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);
typedef BOOL(WINAPI *PFNWGLDXUNLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);
#endif

#endif

class Nv12Render_D3d11va : public QOpenGLFunctions, public VideoRender {
public:
    Nv12Render_D3d11va(ID3D11Device *d3d11Device = nullptr);
    ~Nv12Render_D3d11va() override;

public:
    void initialize(const int width, const int height, const bool horizontal = false,
                    const bool vertical = false) override;
    void render(const decoder_sdk::Frame &frame) override;
    void draw() override;
    QOpenGLFramebufferObject *getFrameBuffer(const QSize &size) override;

private:
    void clearGL();
    bool initializeD3D11();
    bool initializeWGL();
    void cleanupD3D11();
    void cleanupWGL();

private:
    // 纹理同步锁
    QMutex mtx_;

    // D3D11 相关对象
    ID3D11Device *d3d11Device_ = nullptr;
    ID3D11DeviceContext *d3d11Context_ = nullptr;

    // 直接使用源NV12纹理
    ID3D11Texture2D *sourceTexture_ = nullptr;
    UINT sourceTextureIndex_ = 0;

    // WGL 互操作相关
    HANDLE wglD3DDevice_ = nullptr;
    HANDLE wglD3DTextureHandle_ = nullptr;

    // OpenGL纹理（直接绑定到NV12纹理）
    GLuint nv12Texture_ = 0;

    // OpenGL 相关对象
    QOpenGLShaderProgram program_;
    QOpenGLBuffer vbo_;

    // 视频尺寸
    int videoWidth_ = 0;
    int videoHeight_ = 0;

    // 是否需要释放D3D设备
    bool ownD3DDevice_ = false;

    // 资源注册失败标志
    bool resourceRegisteredFailed_ = false;

    // WGL 扩展函数指针
    PFNWGLDXOPENDEVICENVPROC wglDXOpenDeviceNV = nullptr;
    PFNWGLDXCLOSEDEVICENVPROC wglDXCloseDeviceNV = nullptr;
    PFNWGLDXREGISTEROBJECTNVPROC wglDXRegisterObjectNV = nullptr;
    PFNWGLDXUNREGISTEROBJECTNVPROC wglDXUnregisterObjectNV = nullptr;
    PFNWGLDXLOCKOBJECTSNVPROC wglDXLockObjectsNV = nullptr;
    PFNWGLDXUNLOCKOBJECTSNVPROC wglDXUnlockObjectsNV = nullptr;
    
    // 目标纹理（用于复制纹理数组中的特定索引）
    ID3D11Texture2D *targetTexture_ = nullptr;
    UINT targetTextureWidth_ = 0;
    UINT targetTextureHeight_ = 0;
    
    // 新增方法声明
    bool copyTextureData(ID3D11Texture2D *sourceTexture, UINT textureIndex);
};