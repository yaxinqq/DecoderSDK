#pragma once
#ifdef D3D11VA_AVAILABLE
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

class Nv12Render_D3d11va : public VideoRender {
public:
    Nv12Render_D3d11va();
    ~Nv12Render_D3d11va() override;

protected:
    /**
     * @brief 初始化VBO
     * @param horizontal 是否水平镜像
     * @param vertical 是否垂直镜像
     */
    bool initRenderVbo(const bool horizontal, const bool vertical) override;

    /**
     * @brief 初始化渲染Shader
     * @param frame 视频帧
     */
    bool initRenderShader(const decoder_sdk::Frame &frame) override;

    /**
     * @brief 初始化渲染纹理
     * @param frame 视频帧
     */
    bool initRenderTexture(const decoder_sdk::Frame &frame) override;

    /**
     * @brief 初始化硬件帧互操作资源
     * @param frame 视频帧
     */
    bool initInteropsResource(const decoder_sdk::Frame &frame) override;

    /**
     * @brief 渲染视频帧，会绘制在一个FBO上
     * @param frame 视频帧
     */
    bool renderFrame(const decoder_sdk::Frame &frame) override;

private:
    /*
     * @brief 初始化WGL互操作资源
     */
    bool initializeWGLInterop();
    /*
     * @brief 初始化视频帧处理工具
     * 
     * @param width 视频帧宽
     * @param height 视频帧高
     */
    bool initializeVideoProcessor(int width, int height);
    /*
     * @brief 清理申请的资源
     */
    void cleanup();
    /*
     * @brief 加载WDG扩展
     */
    bool loadWGLExtensions();
    /*
     * @brief 创建OpenGL纹理
     */
    bool createRGBTexture();
    /*
     * @brief 将NV12的视频帧，转化为RGB格式的视频帧
     *
     * @param frame 帧数据
     */
    bool processNV12ToRGB(const decoder_sdk::Frame &frame);
    /*
     * @brief D3D Texture 和 OpenGL Texture 互注册（Zero-copy）
     */
    bool registerTextureWithOpenGL(int width, int height);

    /*
     * @brief 绘制视频帧
     *
     * @prarm id RGB纹理
     */
    bool drawFrame(GLuint id);

private:
    // D3D11设备和上下文
    ComPtr<ID3D11Device> d3d11Device_;
    ComPtr<ID3D11DeviceContext> d3d11Context_;

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

    // OpenGL资源
    QOpenGLShaderProgram program_;
    QOpenGLBuffer vbo_;
};

#endif