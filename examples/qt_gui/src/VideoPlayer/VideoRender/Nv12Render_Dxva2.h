#ifndef NV12RENDER_DXVA2_H
#define NV12RENDER_DXVA2_H
#ifdef DXVA2_AVAILABLE

#include "CommonUtils.h"
#include "VideoRender.h"

#include <QDebug>
#include <QOpenGLFunctions>

#ifdef _WIN32
#include <Windows.h>
#include <d3d9.h>
#include <dxva2api.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

#endif

class Nv12Render_Dxva2 : public VideoRender {
public:
    explicit Nv12Render_Dxva2();
    ~Nv12Render_Dxva2() override;

public:
    /*
     * @brief render是否需要重建
     *
     * @return 是否需要重建
     */
    bool shouldRebuild() const;

    /**
     * @brief 得到渲染器名称
     *
     * @return 渲染器名称
     */
    QString renderName() const override;

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
    /**
     * @brief 初始化D3D资源，如果传入wglDevice，则使用传入的，否则使用d3d9Device创建
     *
     * @param d3d9Device D3D9设备
     * @param wglDevice wgl设备
     * @return 是否初始化成功
     */
    bool initializeD3DResource(const ComPtr<IDirect3DDevice9Ex> &d3d9Device,
                               const wgl::WglDeviceRef &wglDevice = wgl::WglDeviceRef());

    /*
     * @brief 检查WGL互操作资源是否有效
     */
    bool checkWGLInterop();
    /*
     * @brief 清理申请的资源
     */
    void cleanup();
    /*
     * @brief 创建RGB纹理（D3D9输出纹理）
     */
    bool createRgbRenderTarget();
    /*
     * @brief 将NV12的视频帧，转化为RGB格式的视频帧
     *
     * @param nv12Surface D3D9 Surface
     */
    bool convertNv12ToRgbStretchRect(LPDIRECT3DSURFACE9 nv12Surface,
                                     const decoder_sdk::Frame &frame);
    /*
     * @brief D3D Texture 和 OpenGL Texture 互注册（Zero-copy）
     *
     * @param width 视频帧宽
     * @param height 视频帧高
     */
    bool registerTextureWithOpenGL(int width, int height);

    /**
     * @brief 确保用来互操作的可读取FBO已初始化完成
     *
     * @return 是否初始化成功
     */
    bool ensureInteropReadFbo();

    /**
     * @brief 拷贝当前的FBO
     *
     * @param width 纹理宽度
     * @param height 纹理高度
     * @return 拷贝是否完成
     */
    bool blitToCurrentFbo(int width, int height);

private:
    // D3D9 related
    ComPtr<IDirect3DDevice9Ex> d3d9Device_;

    // RGB纹理和表面
    ComPtr<IDirect3DSurface9> rgbRenderTarget_;
    HANDLE sharedHandle_ = nullptr;

    // WGL interop handles
    wgl::WglDeviceRef wglD3DDevice_;
    HANDLE wglTextureHandle_ = nullptr;

    // OpenGL resources
    GLuint sharedTexture_ = 0;
    GLuint glInteropReadFbo_ = 0;
    bool horizontalMirror_ = false;
    bool verticalMirror_ = false;

    // 是否需要重建
    bool shouldReBuild_ = false;
};

#endif
#endif // NV12RENDER_DXVA2_H
