#pragma once
#ifdef D3D11VA_AVAILABLE
#include "CommonUtils.h"
#include "VideoRender.h"

#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>

#ifdef _WIN32
#include <Windows.h>
#define D3D11_INTERFACE_DEFINED
#define D3D11_1_INTERFACE_DEFINED
#include <d3d11.h>
#include <d3d11_1.h>
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
    bool initializeVideoProcessor(int width, int height);
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
    /*
     * @brief D3D Texture 和 OpenGL Texture 互注册（Zero-copy）
     */
    bool registerTextureWithOpenGL(int width, int height);

    /*
     * @brief 等待D3D11 GPU操作完成的同步函数
     */
    bool waitForGPUCompletion();

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

    // WGL设备句柄
    wgl::WglDeviceRef wglD3DDevice_;

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

    // 资源缓存结构
    struct ResourceCacheEntry {
        ComPtr<ID3D11Texture2D> inputTexture;
        ComPtr<ID3D11VideoProcessorInputView> inputView;
    };

    std::map<std::pair<uintptr_t, UINT>, ResourceCacheEntry> resourceCache_;

    // 同步机制相关
    ComPtr<ID3D11Query> eventQuery_ = nullptr; // D3D11事件查询对象
    bool enableSyncWorkaround_ = true;         // 是否启用同步解决方案

    // 是否需要重建
    bool shouldReBuild_ = false;
};

#endif