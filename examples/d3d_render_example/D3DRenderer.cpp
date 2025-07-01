#include "D3DRenderer.h"
#include <iostream>
#include <vector>

// 顶点着色器HLSL代码
const char *g_vertexShaderSource = R"(
struct VS_INPUT {
    float3 position : POSITION;
    float2 texCoord : TEXCOORD0;
};

struct VS_OUTPUT {
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT output;
    output.position = float4(input.position, 1.0f);
    output.texCoord = input.texCoord;
    return output;
}
)";

// NV12像素着色器HLSL代码
const char *g_pixelShaderSource = R"(
Texture2D textureY : register(t0);
Texture2D textureUV : register(t1);
SamplerState samplerState : register(s0);

struct PS_INPUT {
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET {
    float y = textureY.Sample(samplerState, input.texCoord).r;
    float2 uv = textureUV.Sample(samplerState, input.texCoord).rg;
    
    // YUV to RGB conversion (BT.709)
    float u = uv.x - 0.5f;
    float v = uv.y - 0.5f;
    
    float r = y + 1.5748f * v;
    float g = y - 0.1873f * u - 0.4681f * v;
    float b = y + 1.8556f * u;
    
    return float4(r, g, b, 1.0f);
}
)";

D3DRenderer::D3DRenderer()
    : m_width(0), m_height(0), m_frameWidth(0), m_frameHeight(0), m_sharedHandle(nullptr)
{
}

D3DRenderer::~D3DRenderer()
{
    Cleanup();
}

bool D3DRenderer::Initialize(HWND hwnd, int width, int height)
{
    m_width = width;
    m_height = height;

    // 创建设备和交换链
    if (!CreateDeviceAndSwapChain(hwnd, width, height)) {
        std::cerr << "Failed to create device and swap chain" << std::endl;
        return false;
    }

    // 创建渲染目标视图
    if (!CreateRenderTargetView()) {
        std::cerr << "Failed to create render target view" << std::endl;
        return false;
    }

    // 创建着色器
    if (!CreateShaders()) {
        std::cerr << "Failed to create shaders" << std::endl;
        return false;
    }

    // 创建顶点缓冲区
    if (!CreateVertexBuffer()) {
        std::cerr << "Failed to create vertex buffer" << std::endl;
        return false;
    }

    // 创建采样器状态
    if (!CreateSamplerState()) {
        std::cerr << "Failed to create sampler state" << std::endl;
        return false;
    }

    std::cout << "D3D11 renderer initialized successfully" << std::endl;
    return true;
}

void D3DRenderer::Cleanup()
{
    // 清理所有COM对象
    m_samplerState.Reset();
    m_srvUV.Reset();
    m_srvY.Reset();
    m_nv12Texture.Reset();
    m_vertexBuffer.Reset();
    m_inputLayout.Reset();
    m_pixelShader.Reset();
    m_vertexShader.Reset();
    m_renderTargetView.Reset();
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();

    if (m_sharedHandle) {
        CloseHandle(m_sharedHandle);
        m_sharedHandle = nullptr;
    }
}

bool D3DRenderer::CreateDeviceAndSwapChain(HWND hwnd, int width, int height)
{
    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.BufferDesc.Width = width;
    swapChainDesc.BufferDesc.Height = height;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 0;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = hwnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                         D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,                  // 适配器
                                               D3D_DRIVER_TYPE_HARDWARE, // 驱动类型
                                               nullptr,                  // 软件模块
                                               createDeviceFlags,        // 创建标志
                                               featureLevels,            // 特性级别数组
                                               ARRAYSIZE(featureLevels), // 特性级别数量
                                               D3D11_SDK_VERSION,        // SDK版本
                                               &swapChainDesc,           // 交换链描述
                                               &m_swapChain,             // 交换链
                                               &m_device,                // 设备
                                               &featureLevel,            // 实际特性级别
                                               &m_context                // 设备上下文
    );

    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D11 device and swap chain. HRESULT: 0x" << std::hex << hr
                  << std::endl;
        return false;
    }

    std::cout << "D3D11 device created with feature level: " << std::hex << featureLevel
              << std::endl;
    return true;
}

bool D3DRenderer::CreateRenderTargetView()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) {
        std::cerr << "Failed to get back buffer" << std::endl;
        return false;
    }

    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_renderTargetView);
    if (FAILED(hr)) {
        std::cerr << "Failed to create render target view" << std::endl;
        return false;
    }

    return true;
}

bool D3DRenderer::CreateShaders()
{
    HRESULT hr;
    ComPtr<ID3DBlob> shaderBlob;
    ComPtr<ID3DBlob> errorBlob;

    // 编译顶点着色器
    hr = D3DCompile(g_vertexShaderSource, strlen(g_vertexShaderSource), nullptr, nullptr, nullptr,
                    "main", "vs_5_0", 0, 0, &shaderBlob, &errorBlob);

    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "Vertex shader compilation error: "
                      << (char *)errorBlob->GetBufferPointer() << std::endl;
        }
        return false;
    }

    hr = m_device->CreateVertexShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(),
                                      nullptr, &m_vertexShader);

    if (FAILED(hr)) {
        std::cerr << "Failed to create vertex shader" << std::endl;
        return false;
    }

    // 创建输入布局
    D3D11_INPUT_ELEMENT_DESC inputElementDesc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};

    hr = m_device->CreateInputLayout(inputElementDesc, ARRAYSIZE(inputElementDesc),
                                     shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(),
                                     &m_inputLayout);

    if (FAILED(hr)) {
        std::cerr << "Failed to create input layout" << std::endl;
        return false;
    }

    // 编译像素着色器
    shaderBlob.Reset();
    errorBlob.Reset();

    hr = D3DCompile(g_pixelShaderSource, strlen(g_pixelShaderSource), nullptr, nullptr, nullptr,
                    "main", "ps_5_0", 0, 0, &shaderBlob, &errorBlob);

    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "Pixel shader compilation error: " << (char *)errorBlob->GetBufferPointer()
                      << std::endl;
        }
        return false;
    }

    hr = m_device->CreatePixelShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(),
                                     nullptr, &m_pixelShader);

    if (FAILED(hr)) {
        std::cerr << "Failed to create pixel shader" << std::endl;
        return false;
    }

    return true;
}

bool D3DRenderer::CreateVertexBuffer()
{
    // 创建全屏四边形顶点数据
    Vertex vertices[] = {
        {{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},  // 左上
        {{1.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},   // 右上
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}}, // 左下
        {{1.0f, -1.0f, 0.0f}, {1.0f, 1.0f}}   // 右下
    };

    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.ByteWidth = sizeof(vertices);
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bufferDesc.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = vertices;

    HRESULT hr = m_device->CreateBuffer(&bufferDesc, &initData, &m_vertexBuffer);
    if (FAILED(hr)) {
        std::cerr << "Failed to create vertex buffer" << std::endl;
        return false;
    }

    return true;
}

bool D3DRenderer::CreateSamplerState()
{
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    HRESULT hr = m_device->CreateSamplerState(&samplerDesc, &m_samplerState);
    if (FAILED(hr)) {
        std::cerr << "Failed to create sampler state" << std::endl;
        return false;
    }

    return true;
}

bool D3DRenderer::CreateNV12Texture(int width, int height)
{
    // 创建NV12纹理
    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_NV12;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    textureDesc.CPUAccessFlags = 0;
    textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr = m_device->CreateTexture2D(&textureDesc, nullptr, &m_nv12Texture);
    if (FAILED(hr)) {
        std::cerr << "Failed to create NV12 texture" << std::endl;
        return false;
    }

    // 创建纹理共享句柄
    ComPtr<IDXGIResource> dxgiShareTexture;
    hr = m_nv12Texture->QueryInterface(__uuidof(IDXGIResource),
                                       (void **)dxgiShareTexture.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "Failed to query DXGI resource interface" << std::endl;
        return false;
    }

    hr = dxgiShareTexture->GetSharedHandle(&m_sharedHandle);
    if (FAILED(hr)) {
        std::cerr << "Failed to get shared handle" << std::endl;
        return false;
    }

    // 创建Y平面的着色器资源视图
    D3D11_SHADER_RESOURCE_VIEW_DESC yPlaneDesc = {};
    yPlaneDesc.Format = DXGI_FORMAT_R8_UNORM;
    yPlaneDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    yPlaneDesc.Texture2D.MipLevels = 1;
    yPlaneDesc.Texture2D.MostDetailedMip = 0;

    hr = m_device->CreateShaderResourceView(m_nv12Texture.Get(), &yPlaneDesc, &m_srvY);
    if (FAILED(hr)) {
        std::cerr << "Failed to create Y plane shader resource view" << std::endl;
        return false;
    }

    // 创建UV平面的着色器资源视图
    D3D11_SHADER_RESOURCE_VIEW_DESC uvPlaneDesc = {};
    uvPlaneDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    uvPlaneDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    uvPlaneDesc.Texture2D.MipLevels = 1;
    uvPlaneDesc.Texture2D.MostDetailedMip = 0;

    hr = m_device->CreateShaderResourceView(m_nv12Texture.Get(), &uvPlaneDesc, &m_srvUV);
    if (FAILED(hr)) {
        std::cerr << "Failed to create UV plane shader resource view" << std::endl;
        return false;
    }

    return true;
}

bool D3DRenderer::UpdateNV12Texture(const decoder_sdk::Frame &frame)
{
    // 获取硬解码输出的D3D11纹理
    ID3D11Texture2D *frameTexture = reinterpret_cast<ID3D11Texture2D *>(frame.data(0));
    if (!frameTexture) {
        std::cerr << "Invalid D3D11 texture from frame" << std::endl;
        return false;
    }

    UINT frameIndex = static_cast<UINT>(reinterpret_cast<intptr_t>(frame.data(1)));

    // 获取帧纹理的设备
    ComPtr<ID3D11Device> frameDevice;
    frameTexture->GetDevice(frameDevice.GetAddressOf());

    ComPtr<ID3D11DeviceContext> frameContext;
    frameDevice->GetImmediateContext(&frameContext);

    // 打开共享纹理
    ComPtr<ID3D11Texture2D> sharedTexture;
    HRESULT hr = frameDevice->OpenSharedResource(m_sharedHandle, __uuidof(ID3D11Texture2D),
                                                 (void **)&sharedTexture);
    if (FAILED(hr)) {
        std::cerr << "Failed to open shared texture" << std::endl;
        return false;
    }

    // 复制纹理数据
    frameContext->CopySubresourceRegion(sharedTexture.Get(), 0,   // 目标纹理和子资源
                                        0, 0, 0,                  // 目标位置
                                        frameTexture, frameIndex, // 源纹理和子资源
                                        nullptr                   // 源区域（nullptr表示整个子资源）
    );

    frameContext->Flush();

    return true;
}

bool D3DRenderer::RenderFrame(const decoder_sdk::Frame &frame)
{
    if (!frame.isValid() || frame.pixelFormat() != decoder_sdk::ImageFormat::kD3d11va) {
        std::cerr << "Invalid frame or unsupported format. Only D3D11VA is supported." << std::endl;
        return false;
    }

    // 检查是否需要重新创建纹理
    if (m_frameWidth != frame.width() || m_frameHeight != frame.height()) {
        // 清理旧纹理
        m_srvUV.Reset();
        m_srvY.Reset();
        m_nv12Texture.Reset();
        if (m_sharedHandle) {
            CloseHandle(m_sharedHandle);
            m_sharedHandle = nullptr;
        }

        // 创建新纹理
        if (!CreateNV12Texture(frame.width(), frame.height())) {
            return false;
        }

        m_frameWidth = frame.width();
        m_frameHeight = frame.height();
    }

    // 更新纹理数据
    if (!UpdateNV12Texture(frame)) {
        return false;
    }

    // 设置渲染状态
    SetRenderState();

    // 绑定着色器和纹理
    m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);

    ID3D11ShaderResourceView *srvs[] = {m_srvY.Get(), m_srvUV.Get()};
    m_context->PSSetShaderResources(0, 2, srvs);

    // 绘制
    m_context->Draw(4, 0);

    // 清理绑定
    ID3D11ShaderResourceView *nullSRVs[] = {nullptr, nullptr};
    m_context->PSSetShaderResources(0, 2, nullSRVs);

    return true;
}

void D3DRenderer::SetRenderState()
{
    // 设置视口
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    m_context->RSSetViewports(1, &viewport);

    // 设置渲染目标
    m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), nullptr);

    // 清除渲染目标
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_context->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);

    // 设置输入布局和顶点缓冲区
    m_context->IASetInputLayout(m_inputLayout.Get());

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    // 设置着色器
    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);

    // 设置采样器
    m_context->PSSetSamplers(0, 1, m_samplerState.GetAddressOf());
}

void D3DRenderer::Present()
{
    if (m_swapChain) {
        HRESULT hr = m_swapChain->Present(1, 0); // VSync enabled

        // 检查设备丢失
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            std::cerr << "D3D11 device lost, HRESULT: 0x" << std::hex << hr << std::endl;

            if (m_device) {
                HRESULT reason = m_device->GetDeviceRemovedReason();
                std::cerr << "Device removed reason: 0x" << std::hex << reason << std::endl;
            }
        }
    }
}

bool D3DRenderer::Resize(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return false;
    }

    m_width = width;
    m_height = height;

    // 释放渲染目标视图
    m_renderTargetView.Reset();

    // 调整交换链缓冲区大小
    HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        std::cerr << "Failed to resize swap chain buffers" << std::endl;
        return false;
    }

    // 重新创建渲染目标视图
    if (!CreateRenderTargetView()) {
        std::cerr << "Failed to recreate render target view after resize" << std::endl;
        return false;
    }

    std::cout << "D3D11 renderer resized to " << width << "x" << height << std::endl;
    return true;
}