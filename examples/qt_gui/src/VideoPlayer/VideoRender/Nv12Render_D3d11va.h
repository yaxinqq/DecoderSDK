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
#include <d3d11_1.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

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
    bool initializeVideoProcessor();
    void cleanup();
    bool loadWGLExtensions();
    bool createRGBTexture(int width, int height);
    bool processNV12ToRGB(const decoder_sdk::Frame &frame);
    bool registerRGBTextureWithOpenGL();
    bool validateOpenGLContext();
    void clearGL();

private:
    // D3D11设备和上下文
    ComPtr<ID3D11Device> d3d11Device_;
    ComPtr<ID3D11DeviceContext> d3d11Context_;
    ComPtr<ID3D11Device1> d3d11Device1_;
    ComPtr<ID3D11DeviceContext1> d3d11Context1_;
    bool ownD3DDevice_ = false;

    // VideoProcessor相关
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoProcessor> videoProcessor_;
    ComPtr<ID3D11VideoProcessorEnumerator> videoProcessorEnum_;
    
    // WGL互操作扩展函数指针
    PFNWGLDXOPENDEVICENVPROC wglDXOpenDeviceNV = nullptr;
    PFNWGLDXCLOSEDEVICENVPROC wglDXCloseDeviceNV = nullptr;
    PFNWGLDXREGISTEROBJECTNVPROC wglDXRegisterObjectNV = nullptr;
    PFNWGLDXUNREGISTEROBJECTNVPROC wglDXUnregisterObjectNV = nullptr;
    PFNWGLDXLOCKOBJECTSNVPROC wglDXLockObjectsNV = nullptr;
    PFNWGLDXUNLOCKOBJECTSNVPROC wglDXUnlockObjectsNV = nullptr;

    // WGL设备句柄
    HANDLE wglD3DDevice_ = nullptr;

    // 输入NV12纹理
    ComPtr<ID3D11Texture2D> inputNV12Texture_ = nullptr;
    ComPtr<ID3D11VideoProcessorInputView> inputView_ = nullptr;
    
    // 输出RGB纹理
    ComPtr<ID3D11Texture2D> outputRGBTexture_ = nullptr;
    ComPtr<ID3D11VideoProcessorOutputView> outputView_ = nullptr;
    HANDLE rgbSharedHandle_ = nullptr;

    // OpenGL纹理
    GLuint glRGBTexture_ = 0;
    HANDLE wglTextureHandle_ = nullptr;

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