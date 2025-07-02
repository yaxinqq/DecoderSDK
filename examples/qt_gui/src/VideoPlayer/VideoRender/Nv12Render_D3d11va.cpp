#include "Nv12Render_D3d11va.h"

#ifdef _WIN32
#include <Windows.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#endif

#include <QDebug>
#include <iostream>

namespace {
// 顶点着色器源码
const char *vsrc = R"(
attribute vec4 vertexIn;
attribute vec2 textureIn;
varying vec2 textureOut;
void main(void)
{
    gl_Position = vertexIn;
    textureOut = textureIn;
}
)";

// NV12 YUV到RGB转换的片段着色器
const char *fsrc = R"(
        precision mediump float;
        uniform sampler2D textureY;
        uniform sampler2D textureUV;

        varying vec2 textureOut;

        void main(void)
        {
            // 采样Y和UV纹理
            float y = texture2D(textureY, textureOut).r;
            vec2 uv = texture2D(textureUV, textureOut).rg;

            // 常量偏移和转换矩阵
            const vec3 yuv2rgb_ofs = vec3(0.0625, 0.5, 0.5);
            const mat3 yuv2rgb_mat = mat3(
                1.16438356,  0.0,           1.79274107,
                1.16438356, -0.21324861, -0.53290932,
                1.16438356,  2.11240178,  0.0
            );

            // YUV到RGB的转换
            vec3 rgb = (vec3(y, uv.r, uv.g) - yuv2rgb_ofs) * yuv2rgb_mat;
            gl_FragColor = vec4(rgb, 1.0);
        }
    )";

const char *calsrc = R"(
        Texture2D<float> texY : register(t0);         // Y plane
        Texture2D<float2> texUV : register(t1);       // UV plane

        RWTexture2D<float> outY : register(u0);       // Output Y
        RWTexture2D<float4> outUV : register(u1);     // Output UV

        [numthreads(16, 16, 1)]
        void CSMain(uint3 id : SV_DispatchThreadID)
        {
            uint2 coord = id.xy;

            // Write Y (full size)
            outY[coord] = texY.Load(int3(coord, 0));

            // UV is subsampled 2x2
            if ((coord.x % 2 == 0) && (coord.y % 2 == 0))
            {
                uint2 uvCoord = coord / uint2(2, 2);
                float2 uv = texUV.Load(int3(uvCoord, 0));
                outUV[uvCoord] = float4(uv.x, uv.y, 0.0, 0.0);
            }
}
    )";
} // namespace

Nv12Render_D3d11va::Nv12Render_D3d11va(ID3D11Device *d3d11Device) : d3d11Device_(d3d11Device)
{
    qDebug() << "Constructor called";

    if (!d3d11Device_) {
        qDebug() << "No D3D11 device provided, creating own";
        initializeD3D11();
        ownD3DDevice_ = true;
    }

    if (d3d11Device_) {
        d3d11Device_->GetImmediateContext(&d3d11Context_);

        qDebug() << "D3D11 context obtained";
    }
}

Nv12Render_D3d11va::~Nv12Render_D3d11va()
{
    qDebug() << "Destructor called";

    // 清理所有资源
    cleanup();

    // 清理WGL设备
    if (wglD3DDevice_) {
        qDebug() << "Closing WGL D3D device";
        wglDXCloseDeviceNV(wglD3DDevice_);
        wglD3DDevice_ = nullptr;
    }

    // 清理OpenGL资源
    vbo_.destroy();

    // 清理D3D11上下文
    if (d3d11Context_) {
        d3d11Context_.Reset();
    }

    // 如果拥有D3D设备，则释放它
    if (ownD3DDevice_ && d3d11Device_) {
        d3d11Device_.Reset();
    }
}

void Nv12Render_D3d11va::initialize(const int width, const int height, const bool horizontal,
                                    const bool vertical)
{
    qDebug() << "Initialize called with size:" << width << "x" << height;

    initializeOpenGLFunctions();

    videoWidth_ = width;
    videoHeight_ = height;
    flipHorizontal_ = horizontal;
    flipVertical_ = vertical;

    // 验证OpenGL上下文
    if (!validateOpenGLContext()) {
        qDebug() << "Invalid OpenGL context";
        return;
    }

    // 初始化WGL互操作
    if (!initializeWGLInterop()) {
        qDebug() << "Failed to initialize WGL interop";
        return;
    }

    // 编译着色器
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vsrc);
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fsrc);
    program_.link();

    // 设置顶点数据（根据翻转参数调整纹理坐标）
    GLfloat points[] = {
        // 位置坐标
        -1.0f,
        1.0f,
        1.0f,
        1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        -1.0f,

        // 纹理坐标（根据翻转参数调整）
        flipHorizontal_ ? 1.0f : 0.0f,
        flipVertical_ ? 1.0f : 0.0f,
        flipHorizontal_ ? 0.0f : 1.0f,
        flipVertical_ ? 1.0f : 0.0f,
        flipHorizontal_ ? 1.0f : 0.0f,
        flipVertical_ ? 0.0f : 1.0f,
        flipHorizontal_ ? 0.0f : 1.0f,
        flipVertical_ ? 0.0f : 1.0f,
    };

    vbo_.create();
    vbo_.bind();
    vbo_.allocate(points, sizeof(points));

    clearGL();
    qDebug() << "Initialize completed successfully";
}

void Nv12Render_D3d11va::render(const decoder_sdk::Frame &frame)
{
    if (!frame.isValid() || frame.pixelFormat() != decoder_sdk::ImageFormat::kD3d11va) {
        qDebug() << "Invalid frame or wrong pixel format";
        clearGL();
        return;
    }

    // 创建NV12共享纹理
    if (!createNV12SharedTextures(frame)) {
        qDebug() << "Failed to create NV12 shared textures";
        return;
    }

    qDebug() << "Frame rendered successfully";
}

void Nv12Render_D3d11va::draw()
{
    if (!glTextureY_ || !glTextureUV_ || !program_.isLinked()) {
        qDebug() << "Not ready for drawing - missing textures or shader";
        clearGL();
        return;
    }

    if (!wglTextureHandleY_ || !wglTextureHandleUV_) {
        qDebug() << "WGL handles not available";
        clearGL();
        return;
    }

    qDebug() << "Drawing with Y texture:" << glTextureY_ << "UV texture:" << glTextureUV_;

    HANDLE handles[] = {wglTextureHandleY_, wglTextureHandleUV_};
    if (!wglDXLockObjectsNV(wglD3DDevice_, 2, handles)) {
        qDebug() << "Failed to lock WGL objects";
        clearGL();
        return;
    }

    program_.bind();
    vbo_.bind();

    // 绑定Y平面纹理到纹理单元0
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, glTextureY_);
    program_.setUniformValue("textureY", 0);

    // 绑定UV平面纹理到纹理单元1
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, glTextureUV_);
    program_.setUniformValue("textureUV", 1);

    // 检查OpenGL错误
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        qDebug() << "OpenGL error after binding textures:" << error;
    }

    // 设置顶点属性
    program_.enableAttributeArray("vertexIn");
    program_.enableAttributeArray("textureIn");
    program_.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 2, 0);
    program_.setAttributeBuffer("textureIn", GL_FLOAT, 2 * 4 * sizeof(GLfloat), 2, 0);

    // 绘制
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // 检查OpenGL错误
    error = glGetError();
    if (error != GL_NO_ERROR) {
        qDebug() << "OpenGL error after drawing:" << error;
    }

    program_.disableAttributeArray("vertexIn");
    program_.disableAttributeArray("textureIn");
    program_.release();

    // 解锁WGL对象
    if (!wglDXUnlockObjectsNV(wglD3DDevice_, 2, handles)) {
        qDebug() << "Failed to unlock WGL objects";
    }

    qDebug() << "Draw completed";
}

QOpenGLFramebufferObject *Nv12Render_D3d11va::getFrameBuffer(const QSize &size)
{
    return nullptr;
}

bool Nv12Render_D3d11va::initializeD3D11()
{
    qDebug() << "Initializing D3D11";

    HRESULT hr;
    D3D_FEATURE_LEVEL featureLevel;

    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                           D3D11_SDK_VERSION, &d3d11Device_, &featureLevel, &d3d11Context_);

    if (FAILED(hr)) {
        qDebug() << "Failed to create D3D11 device, HRESULT:" << hr;
        return false;
    }

    qDebug() << "D3D11 device created successfully";
    return true;
}

bool Nv12Render_D3d11va::initializeWGLInterop()
{
    qDebug() << "Initializing WGL interop";

    // 加载WGL扩展
    if (!loadWGLExtensions()) {
        qDebug() << "Failed to load WGL extensions";
        return false;
    }

    qDebug() << "WGL extensions loaded successfully";

    // 验证 D3D11 设备状态
    if (!d3d11Device_) {
        qDebug() << "D3D11 device is null";
        return false;
    }

    // 检查设备是否有效
    HRESULT hr = d3d11Device_->GetDeviceRemovedReason();
    if (FAILED(hr)) {
        qDebug() << "D3D11 device is in error state, HRESULT:" << hr;
        return false;
    }

    qDebug() << "D3D11 device is valid";

    // 打开D3D设备用于互操作
    wglD3DDevice_ = wglDXOpenDeviceNV(d3d11Device_.Get());
    if (!wglD3DDevice_) {
        DWORD error = GetLastError();
        qDebug() << "Failed to open D3D device for WGL interop, error:" << error;
        return false;
    }

    qDebug() << "WGL interop initialized successfully, handle:" << wglD3DDevice_;
    return true;
}

void Nv12Render_D3d11va::cleanup()
{
    qDebug() << "Cleaning up resources";

    // 清理WGL注册的纹理
    if (wglTextureHandleY_ && wglD3DDevice_) {
        qDebug() << "Unregistering WGL Y texture handle:" << wglTextureHandleY_;
        wglDXUnregisterObjectNV(wglD3DDevice_, wglTextureHandleY_);
        wglTextureHandleY_ = nullptr;
    }

    if (wglTextureHandleUV_ && wglD3DDevice_) {
        qDebug() << "Unregistering WGL UV texture handle:" << wglTextureHandleUV_;
        wglDXUnregisterObjectNV(wglD3DDevice_, wglTextureHandleUV_);
        wglTextureHandleUV_ = nullptr;
    }

    // 清理OpenGL纹理
    if (glTextureY_) {
        qDebug() << "Deleting OpenGL Y texture:" << glTextureY_;
        glDeleteTextures(1, &glTextureY_);
        glTextureY_ = 0;
    }

    if (glTextureUV_) {
        qDebug() << "Deleting OpenGL UV texture:" << glTextureUV_;
        glDeleteTextures(1, &glTextureUV_);
        glTextureUV_ = 0;
    }

    // 清理D3D11纹理
    if (nv12Texture_) {
        qDebug() << "Releasing D3D11 NV12 texture";
        nv12Texture_->Release();
        nv12Texture_ = nullptr;
    }

    currentWidth_ = 0;
    currentHeight_ = 0;
}

bool Nv12Render_D3d11va::loadWGLExtensions()
{
    wglDXOpenDeviceNV = (PFNWGLDXOPENDEVICENVPROC)wglGetProcAddress("wglDXOpenDeviceNV");
    wglDXCloseDeviceNV = (PFNWGLDXCLOSEDEVICENVPROC)wglGetProcAddress("wglDXCloseDeviceNV");
    wglDXRegisterObjectNV =
        (PFNWGLDXREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXRegisterObjectNV");
    wglDXUnregisterObjectNV =
        (PFNWGLDXUNREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXUnregisterObjectNV");
    wglDXLockObjectsNV = (PFNWGLDXLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXLockObjectsNV");
    wglDXUnlockObjectsNV = (PFNWGLDXUNLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXUnlockObjectsNV");

    return wglDXOpenDeviceNV && wglDXCloseDeviceNV && wglDXRegisterObjectNV &&
           wglDXUnregisterObjectNV && wglDXLockObjectsNV && wglDXUnlockObjectsNV;
}

bool Nv12Render_D3d11va::createNV12SharedTextures(const decoder_sdk::Frame &frame)
{
    // 从Frame中提取D3D11纹理
    ID3D11Texture2D *sourceTexture = reinterpret_cast<ID3D11Texture2D *>(frame.data(0));
    if (!sourceTexture || !d3d11Context_ || !wglD3DDevice_) {
        qDebug() << "Missing required resources";
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc;
    sourceTexture->GetDesc(&srcDesc);

    // 检查是否需要重新创建纹理
    if (!yTexture_ || !uvTexture_ || currentWidth_ != srcDesc.Width ||
        currentHeight_ != srcDesc.Height) {
        qDebug() << "Creating new separated textures";

        cleanup();

        currentWidth_ = srcDesc.Width;
        currentHeight_ = srcDesc.Height;

        // 创建共享的NV12纹理
        D3D11_TEXTURE2D_DESC nv12Desc{};
        nv12Desc.Width = srcDesc.Width;
        nv12Desc.Height = srcDesc.Height;
        nv12Desc.Format = srcDesc.Format;
        nv12Desc.ArraySize = 1;
        nv12Desc.MipLevels = 1;
        nv12Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        nv12Desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        nv12Desc.Usage = D3D11_USAGE_DEFAULT;
        nv12Desc.SampleDesc = {1, 0};

        HRESULT hr = d3d11Device_->CreateTexture2D(&nv12Desc, nullptr, &nv12Texture_);
        if (FAILED(hr)) {
            qDebug() << "Failed to create NV12 texture";
            return false;
        }

        // 创建分离的Y和UV纹理
        if (!createSeparatedTextures(currentWidth_, currentHeight_)) {
            return false;
        }

        // 创建NV12 SRV
        if (!createNV12SRVs()) {
            return false;
        }

        // 创建计算着色器
        if (!createPlaneSeparationShader()) {
            return false;
        }

        // 注册到OpenGL
        if (!registerPlanesWithOpenGL()) {
            return false;
        }
    }

    // 使用共享资源机制复制纹理
    UINT frameIndex = static_cast<UINT>(reinterpret_cast<intptr_t>(frame.data(1)));

    // 获取源纹理的共享句柄
    ComPtr<IDXGIResource> dxgiResource;
    HRESULT hr = sourceTexture->QueryInterface(
        __uuidof(IDXGIResource), reinterpret_cast<void **>(dxgiResource.GetAddressOf()));
    if (FAILED(hr)) {
        qDebug() << "Failed to query DXGI resource interface, HRESULT:" << QString::number(hr, 16);
        return false;
    }

    HANDLE sharedHandle = nullptr;
    hr = dxgiResource->GetSharedHandle(&sharedHandle);
    if (FAILED(hr)) {
        qDebug() << "Failed to get shared handle, HRESULT:" << QString::number(hr, 16);
        return false;
    }

    // 在当前设备上打开共享纹理
    ComPtr<ID3D11Texture2D> sharedSourceTexture;
    hr = d3d11Device_->OpenSharedResource(
        sharedHandle, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void **>(sharedSourceTexture.GetAddressOf()));
    if (FAILED(hr)) {
        qDebug() << "Failed to open shared resource, HRESULT:" << QString::number(hr, 16);
        return false;
    }

    // 验证共享纹理
    if (!sharedSourceTexture) {
        qDebug() << "Shared source texture is null";
        return false;
    }

    // 获取共享纹理描述
    D3D11_TEXTURE2D_DESC sharedDesc;
    sharedSourceTexture->GetDesc(&sharedDesc);

    // 验证 frameIndex 是否有效
    if (frameIndex >= sharedDesc.ArraySize) {
        qDebug() << "Invalid frame index:" << frameIndex << "Array size:" << sharedDesc.ArraySize;
        return false;
    }

    // 锁定WGL对象
    HANDLE handles[] = {wglTextureHandleY_, wglTextureHandleUV_};
    if (!wglDXLockObjectsNV(wglD3DDevice_, 2, handles)) {
        DWORD error = GetLastError();
        qDebug() << "Failed to lock WGL objects, error:" << error;
        return false;
    }

    // 使用共享纹理进行复制
    qDebug() << "Copying shared texture - frameIndex:" << frameIndex;
    d3d11Context_->CopySubresourceRegion(nv12Texture_.Get(), 0, 0, 0, 0, sharedSourceTexture.Get(),
                                         frameIndex, nullptr);

    // 执行平面分离
    if (!separatePlanes()) {
        qDebug() << "Failed to separate planes";
        return false;
    }

    // 解锁WGL对象
    if (!wglDXUnlockObjectsNV(wglD3DDevice_, 2, handles)) {
        DWORD error = GetLastError();
        qDebug() << "Failed to unlock WGL objects, error:" << error;
        return false;
    }

    qDebug() << "NV12 shared textures created and updated successfully";
    return true;
}

bool Nv12Render_D3d11va::createSeparatedTextures(int width, int height)
{
    // 创建Y平面纹理
    D3D11_TEXTURE2D_DESC descY = {};
    descY.Width = width;
    descY.Height = height;
    descY.MipLevels = 1;
    descY.ArraySize = 1;
    descY.Format = DXGI_FORMAT_R8_UNORM;
    descY.SampleDesc.Count = 1;
    descY.SampleDesc.Quality = 0;
    descY.Usage = D3D11_USAGE_DEFAULT;
    descY.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    descY.CPUAccessFlags = 0;
    descY.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr = d3d11Device_->CreateTexture2D(&descY, nullptr, &yTexture_);
    if (FAILED(hr)) {
        qDebug() << "Failed to create Y texture, HRESULT:" << hr;
        return false;
    }

    // 创建UV平面纹理 - 使用BGRA格式而不是RG
    D3D11_TEXTURE2D_DESC descUV = {};
    descUV.Width = width / 2;
    descUV.Height = height / 2;
    descUV.MipLevels = 1;
    descUV.ArraySize = 1;
    descUV.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    descUV.SampleDesc.Count = 1;
    descUV.SampleDesc.Quality = 0;
    descUV.Usage = D3D11_USAGE_DEFAULT;
    descUV.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    descUV.CPUAccessFlags = 0;
    descUV.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    hr = d3d11Device_->CreateTexture2D(&descUV, nullptr, &uvTexture_);
    if (FAILED(hr)) {
        qDebug() << "Failed to create UV texture, HRESULT:" << hr;
        return false;
    }

    // 获取共享句柄
    ComPtr<IDXGIResource> yDxgiResource;
    hr = yTexture_->QueryInterface(__uuidof(IDXGIResource), (void **)&yDxgiResource);
    if (FAILED(hr)) {
        qDebug() << "Failed to query Y texture DXGI resource";
        return false;
    }
    hr = yDxgiResource->GetSharedHandle(&ySharedHandle_);
    if (FAILED(hr)) {
        qDebug() << "Failed to get Y texture shared handle";
        return false;
    }

    ComPtr<IDXGIResource> uvDxgiResource;
    hr = uvTexture_->QueryInterface(__uuidof(IDXGIResource), (void **)&uvDxgiResource);
    if (FAILED(hr)) {
        qDebug() << "Failed to query UV texture DXGI resource";
        return false;
    }
    hr = uvDxgiResource->GetSharedHandle(&uvSharedHandle_);
    if (FAILED(hr)) {
        qDebug() << "Failed to get UV texture shared handle";
        return false;
    }

    // 创建UAV
    hr = d3d11Device_->CreateUnorderedAccessView(yTexture_.Get(), nullptr, &yUav_);
    if (FAILED(hr)) {
        qDebug() << "Failed to create Y UAV";
        return false;
    }

    hr = d3d11Device_->CreateUnorderedAccessView(uvTexture_.Get(), nullptr, &uvUav_);
    if (FAILED(hr)) {
        qDebug() << "Failed to create UV UAV";
        return false;
    }

    qDebug() << "Successfully created separated Y and UV textures";
    return true;
}

bool Nv12Render_D3d11va::createPlaneSeparationShader()
{
    ComPtr<ID3DBlob> shaderBlob;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompile(calsrc, strlen(calsrc), nullptr, nullptr, nullptr, "CSMain", "cs_5_0",
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, &shaderBlob, &errorBlob);

    if (FAILED(hr)) {
        if (errorBlob) {
            qDebug() << "Shader compilation error:" << (char *)errorBlob->GetBufferPointer();
        }
        return false;
    }

    hr = d3d11Device_->CreateComputeShader(shaderBlob->GetBufferPointer(),
                                           shaderBlob->GetBufferSize(), nullptr,
                                           planeSeparationCS_.GetAddressOf());

    if (FAILED(hr)) {
        qDebug() << "Failed to create compute shader";
        return false;
    }

    // d3d11Context_->CSSetShader(planeSeparationCS_.Get(), nullptr, 0);
    return true;
}

bool Nv12Render_D3d11va::separatePlanes()
{
    if (!planeSeparationCS_ || !nv12YSrv_ || !nv12UvSrv_ || !yUav_ || !uvUav_) {
        qDebug() << "Missing resources for plane separation";
        return false;
    }

    // 设置计算着色器
    d3d11Context_->CSSetShader(planeSeparationCS_.Get(), nullptr, 0);

    // 设置SRV
    ID3D11ShaderResourceView *srvs[] = {nv12YSrv_.Get(), nv12UvSrv_.Get()};
    d3d11Context_->CSSetShaderResources(0, 2, srvs);

    // 设置UAV
    ID3D11UnorderedAccessView *uavs[] = {yUav_.Get(), uvUav_.Get()};
    d3d11Context_->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

    // 执行计算着色器
    d3d11Context_->Dispatch((currentWidth_ + 15) / 16, (currentHeight_ + 15) / 16, 1);

    // 清理绑定
    ID3D11ShaderResourceView *nullSRVs[2] = {nullptr, nullptr};
    d3d11Context_->CSSetShaderResources(0, 2, nullSRVs);
    ID3D11UnorderedAccessView *nullUAVs[2] = {nullptr, nullptr};
    d3d11Context_->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
    d3d11Context_->CSSetShader(nullptr, nullptr, 0);

    d3d11Context_->Flush();

    return true;
}

bool Nv12Render_D3d11va::createNV12SRVs()
{
    if (!nv12Texture_) {
        qDebug() << "NV12 texture is null";
        return false;
    }

    // 创建Y平面SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescY = {};
    srvDescY.Format = DXGI_FORMAT_R8_UNORM;
    srvDescY.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDescY.Texture2D.MostDetailedMip = 0;
    srvDescY.Texture2D.MipLevels = 1;

    HRESULT hr = d3d11Device_->CreateShaderResourceView(nv12Texture_.Get(), &srvDescY, &nv12YSrv_);
    if (FAILED(hr)) {
        qDebug() << "Failed to create NV12 Y SRV";
        return false;
    }

    // 创建UV平面SRV（使用PlaneSlice）
    ComPtr<ID3D11Device3> device3;
    hr = d3d11Device_->QueryInterface(__uuidof(ID3D11Device3), (void **)&device3);
    if (SUCCEEDED(hr)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC1 srvDescUV = {};
        srvDescUV.Format = DXGI_FORMAT_R8G8_UNORM;
        srvDescUV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDescUV.Texture2D.MostDetailedMip = 0;
        srvDescUV.Texture2D.MipLevels = 1;
        srvDescUV.Texture2D.PlaneSlice = 1; // UV平面

        ComPtr<ID3D11ShaderResourceView1> srvUV1;
        hr = device3->CreateShaderResourceView1(nv12Texture_.Get(), &srvDescUV, &srvUV1);
        if (SUCCEEDED(hr)) {
            hr = srvUV1->QueryInterface(__uuidof(ID3D11ShaderResourceView), (void **)&nv12UvSrv_);
        }
    }

    if (FAILED(hr)) {
        qDebug() << "Failed to create NV12 UV SRV";
        return false;
    }

    return true;
}

bool Nv12Render_D3d11va::registerPlanesWithOpenGL()
{
    // 创建OpenGL纹理
    glGenTextures(1, &glTextureY_);
    glGenTextures(1, &glTextureUV_);

    // 设置Y平面纹理参数 - 使用 GL_R8 格式
    glBindTexture(GL_TEXTURE_2D, glTextureY_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, currentWidth_, currentHeight_, 0, GL_RED,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 设置UV平面纹理参数 - 使用 GL_RG8 格式
    glBindTexture(GL_TEXTURE_2D, glTextureUV_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, currentWidth_, currentHeight_, 0, GL_RG,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 检查OpenGL错误
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        qDebug() << "OpenGL error after texture creation:" << error;
        return false;
    }

    // 注册Y平面到WGL - 使用原始纹理
    wglTextureHandleY_ = wglDXRegisterObjectNV(wglD3DDevice_, yTexture_.Get(), glTextureY_,
                                               GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
    if (!wglTextureHandleY_) {
        DWORD error = GetLastError();
        qDebug() << "Failed to register Y plane with WGL, error:" << error;
        return false;
    }

    // 注册UV平面到WGL - 使用原始纹理
    wglTextureHandleUV_ = wglDXRegisterObjectNV(wglD3DDevice_, uvTexture_.Get(), glTextureUV_,
                                                GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
    if (!wglTextureHandleUV_) {
        DWORD error = GetLastError();
        qDebug() << "Failed to register UV plane with WGL, error:" << error;
        wglDXUnregisterObjectNV(wglD3DDevice_, wglTextureHandleY_);
        wglTextureHandleY_ = nullptr;
        return false;
    }

    qDebug() << "Successfully registered Y and UV planes with WGL";
    qDebug() << "Y handle:" << wglTextureHandleY_ << "UV handle:" << wglTextureHandleUV_;
    return true;
}

bool Nv12Render_D3d11va::validateOpenGLContext()
{
    const GLubyte *version = glGetString(GL_VERSION);
    if (!version) {
        qDebug() << "Failed to get OpenGL version";
        return false;
    }

    qDebug() << "OpenGL version:" << reinterpret_cast<const char *>(version);
    return true;
}

void Nv12Render_D3d11va::clearGL()
{
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

// 工厂函数
VideoRender *createNv12Render_D3d11va(void *d3d11Device)
{
    return new Nv12Render_D3d11va(static_cast<ID3D11Device *>(d3d11Device));
}