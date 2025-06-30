#pragma once

#include "VideoRender.h"

#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>

#ifdef _WIN32
#include <GL/gl.h>
#include <Windows.h>
#define D3D11_INTERFACE_DEFINED
#define D3D11_1_INTERFACE_DEFINED
#include <d3d11.h>
#include <d3d11_3.h> // 添加D3D 11.1支持
#include <dxgi.h>

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
    bool initializeD3D11();
    bool initializeWGLInterop();
    void cleanup();
    bool loadWGLExtensions();
    bool createNV12SharedTextures(ID3D11Texture2D *sourceTexture);
    bool createPlaneShaderResourceViews(ID3D11Texture2D *nv12Texture);
    bool registerPlanesWithOpenGL();
    bool validateOpenGLContext();
    void clearGL();

    // YUV到RGB转换函数
    static const char *getYUVToRGBShaderCode();

private:
    // D3D11设备和上下文
    ID3D11Device *d3d11Device_;
    ID3D11Device3 *d3d11Device3_; // 添加D3D11.1设备接口
    ID3D11DeviceContext *d3d11Context_;
    bool ownD3DDevice_ = false;

    // WGL互操作扩展函数指针
    PFNWGLDXOPENDEVICENVPROC wglDXOpenDeviceNV = nullptr;
    PFNWGLDXCLOSEDEVICENVPROC wglDXCloseDeviceNV = nullptr;
    PFNWGLDXREGISTEROBJECTNVPROC wglDXRegisterObjectNV = nullptr;
    PFNWGLDXUNREGISTEROBJECTNVPROC wglDXUnregisterObjectNV = nullptr;
    PFNWGLDXLOCKOBJECTSNVPROC wglDXLockObjectsNV = nullptr;
    PFNWGLDXUNLOCKOBJECTSNVPROC wglDXUnlockObjectsNV = nullptr;

    // NV12双平面纹理资源
    HANDLE wglD3DDevice_ = nullptr;

    // D3D11 NV12纹理和SRV
    ID3D11Texture2D *nv12Texture_ = nullptr;
    ID3D11Texture2D *yTexture_ = nullptr;        // 添加Y平面纹理
    ID3D11Texture2D *uvTexture_ = nullptr;       // 添加UV平面纹理
    ID3D11ShaderResourceView1 *ySRV_ = nullptr;  // Y平面SRV
    ID3D11ShaderResourceView1 *uvSRV_ = nullptr; // UV平面SRV

    // OpenGL纹理
    GLuint glTextureY_ = 0;  // Y平面OpenGL纹理
    GLuint glTextureUV_ = 0; // UV平面OpenGL纹理

    // WGL句柄
    HANDLE wglTextureHandleY_ = nullptr;  // Y平面WGL句柄
    HANDLE wglTextureHandleUV_ = nullptr; // UV平面WGL句柄

    // 基本参数
    int videoWidth_ = 0;
    int videoHeight_ = 0;
    int currentWidth_ = 0;
    int currentHeight_ = 0;
    bool flipHorizontal_ = false;
    bool flipVertical_ = false;

    // OpenGL资源
    QOpenGLShaderProgram program_;
    QOpenGLBuffer vbo_;
};