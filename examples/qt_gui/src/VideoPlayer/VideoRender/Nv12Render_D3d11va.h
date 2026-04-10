#pragma once
#ifdef D3D11VA_AVAILABLE
#include "CommonUtils.h"
#include "VideoRender.h"

#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>

#ifdef _WIN32
#include <Windows.h>
#define D3D11_INTERFACE_DEFINED
#define D3D11_1_INTERFACE_DEFINED
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
#endif

class Nv12Render_D3d11va : public VideoRender {
public:
    Nv12Render_D3d11va();
    ~Nv12Render_D3d11va() override;

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

    /**
     * @brief 清理所有相关的资源（特定API资源 + 对应的OpenGL资源）
     */
    void cleanupAllResources() override;

private:
    /**
     * @brief 初始化D3D资源，如果传入wglDevice，则使用传入的，否则使用d3d11Device创建
     *
     * @param d3d11Device D3D11设备
     * @param wglDevice wgl设备
     * @return 是否初始化成功
     */
    bool initializeD3DResource(const ComPtr<ID3D11Device> &d3d11Device,
                               const wgl::WglDeviceRef &wglDevice = wgl::WglDeviceRef());

    /*
     * @brief 检查WGL互操作资源是否有效
     */
    bool checkWGLInterop();
    /*
     * @brief 初始化视频帧处理工具
     *
     * @param width 视频帧宽
     * @param height 视频帧高
     */
    bool initializeNv12Converter();
    /*
     * @brief 清理申请的资源
     */
    void cleanup();
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

    /**
     * @brief 确保输出纹理和互操作资源初始化完成
     *
     * @param width 纹理宽度
     * @param height 纹理高度
     * @return 是否初始化成功
     */
    bool ensureOutputTextureAndInterop(int width, int height);

    /**
     * @brief 确保输入的着色器资源已初始化完成
     *
     * @param cmdContext D3D11设备上下文
     * @param sourceTexture 源纹理
     * @param arraySlice 纹理队列索引
     * @param outYSrv 输出的Y平面着色器资源视图
     * @param outUVSrv 输出的UV平面着色器资源视图
     * @return 是否初始化成功
     */
    bool ensureInputShaderResources(ID3D11DeviceContext *cmdContext, ID3D11Texture2D *sourceTexture,
                                    UINT arraySlice, ComPtr<ID3D11ShaderResourceView> &outYSrv,
                                    ComPtr<ID3D11ShaderResourceView> &outUVSrv);

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

    /*
     * @brief D3D Texture 和 OpenGL Texture 互注册
     *
     * @param width 纹理宽度
     * @param height 纹理高度
     */
    bool registerTextureWithOpenGL(int width, int height);
    bool waitForExecuteComplete();

private:
    // D3D11设备和上下文
    ComPtr<ID3D11Device> d3d11Device_;
    ComPtr<ID3D11DeviceContext> d3d11Context_;
    ComPtr<ID3D11DeviceContext> d3d11DeferredContext_;

    ComPtr<ID3D11VertexShader> nv12VertexShader_;
    ComPtr<ID3D11PixelShader> nv12PixelShader_;
    ComPtr<ID3D11PixelShader> p010PixelShader_;
    ComPtr<ID3D11SamplerState> nv12Sampler_;
    ComPtr<ID3D11Buffer> texScaleCb_;
    ComPtr<ID3D11Device5> d3d11Device5_;
    ComPtr<ID3D11DeviceContext4> d3d11Context4_;
    ComPtr<ID3D11Fence> executeFinishedFence_;
    ComPtr<ID3D11Query> executeFinishedQuery_;
    HANDLE executeFenceEvent_ = nullptr;
    UINT64 executeFenceValue_ = 0;
    bool useFenceSync_ = false;

    // WGL设备句柄
    wgl::WglDeviceRef wglD3DDevice_;

    int outputWidth_ = 0;
    int outputHeight_ = 0;

    // 输出RGB纹理
    ComPtr<ID3D11Texture2D> outputRGBTexture_ = nullptr;
    ComPtr<ID3D11RenderTargetView> outputRTV_ = nullptr;

    // OpenGL纹理
    GLuint glRGBTexture_ = 0;
    HANDLE wglTextureHandle_ = nullptr;

    // 用于交换的FBO
    GLuint glInteropReadFbo_ = 0;
    bool horizontalMirror_ = false;
    bool verticalMirror_ = false;

    // 不同逻辑设备
    ComPtr<ID3D11Texture2D> inputCopyTexture_ = nullptr;
    ComPtr<ID3D11ShaderResourceView> inputCopyYSrv_ = nullptr;
    ComPtr<ID3D11ShaderResourceView> inputCopyUVSrv_ = nullptr;

    // 是否需要重建
    bool shouldReBuild_ = false;
};

#endif
