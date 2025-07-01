#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>
#include <memory>

#include "decodersdk/frame.h"

using Microsoft::WRL::ComPtr;

class D3DRenderer {
public:
    D3DRenderer();
    ~D3DRenderer();

    // 初始化D3D11设备和交换链
    bool Initialize(HWND hwnd, int width, int height);

    // 清理资源
    void Cleanup();

    // 渲染帧（仅支持D3D11VA硬解码）
    bool RenderFrame(const decoder_sdk::Frame &frame);

    // 呈现画面
    void Present();

    // 调整窗口大小
    bool Resize(int width, int height);

private:
    // 设备初始化相关
    bool CreateDeviceAndSwapChain(HWND hwnd, int width, int height);
    bool CreateRenderTargetView();
    bool CreateShaders();
    bool CreateVertexBuffer();
    bool CreateSamplerState();

    // NV12纹理和渲染相关
    bool CreateNV12Texture(int width, int height);
    bool UpdateNV12Texture(const decoder_sdk::Frame &frame);

    // 渲染状态设置
    void SetRenderState();

    // D3D11核心对象
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_renderTargetView;

    // 着色器
    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11InputLayout> m_inputLayout;

    // 缓冲区
    ComPtr<ID3D11Buffer> m_vertexBuffer;

    // NV12纹理相关
    ComPtr<ID3D11Texture2D> m_nv12Texture;
    ComPtr<ID3D11ShaderResourceView> m_srvY;  // Y平面视图
    ComPtr<ID3D11ShaderResourceView> m_srvUV; // UV平面视图
    HANDLE m_sharedHandle;

    // 采样器
    ComPtr<ID3D11SamplerState> m_samplerState;

    // 渲染参数
    int m_width;
    int m_height;
    int m_frameWidth;
    int m_frameHeight;

    // 顶点结构
    struct Vertex {
        float position[3];
        float texCoord[2];
    };
};