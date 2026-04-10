#ifdef D3D11VA_AVAILABLE
#include "Nv12Render_D3d11va.h"

#ifdef QSV_AVAILABLE
#include "mfxstructures.h"
#endif

#ifdef _WIN32
#include <Windows.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#endif
#include <mutex>

namespace {
    std::mutex g_d3d11ImmediateExecMutex;

    const char *nv12VsHlsl = R"(
struct VSOut {
    float4 position : SV_Position;
    float2 texcoord  : TEXCOORD0;
};

VSOut main(uint vertexId : SV_VertexID)
{
    VSOut o;

    float2 pos;
    float2 uv;
    if (vertexId == 0) {
        pos = float2(-1.0f, -1.0f);
        uv  = float2(0.0f, 1.0f);
    } else if (vertexId == 1) {
        pos = float2(-1.0f, 3.0f);
        uv  = float2(0.0f, -1.0f);
    } else {
        pos = float2(3.0f, -1.0f);
        uv  = float2(2.0f, 1.0f);
    }

    o.position = float4(pos, 0.0f, 1.0f);
    o.texcoord = uv;
    return o;
}
)";

    const char *nv12PsHlsl = R"(
Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState sampLinear : register(s0);
cbuffer TexScaleCB : register(b0)
{
    float2 texScale;
    float2 _pad0;
};

struct PSIn {
    float4 position : SV_Position;
    float2 texcoord  : TEXCOORD0;
};

float4 main(PSIn i) : SV_Target
{
    float2 tc = saturate(i.texcoord) * texScale;
    float y = texY.Sample(sampLinear, tc);
    float2 uv = texUV.Sample(sampLinear, tc);

    y = 1.16438356f * (y - (16.0f / 255.0f));
    float u = uv.x - 0.5f;
    float v = uv.y - 0.5f;

    float r = y + 1.79274107f * v;
    float g = y - 0.21324861f * u - 0.53290933f * v;
    float b = y + 2.11240179f * u;

    return float4(saturate(r), saturate(g), saturate(b), 1.0f);
}
)";

    const char *p010PsHlsl = R"(
Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState sampLinear : register(s0);
cbuffer TexScaleCB : register(b0)
{
    float2 texScale;
    float2 _pad0;
};

struct PSIn {
    float4 position : SV_Position;
    float2 texcoord  : TEXCOORD0;
};

float4 main(PSIn i) : SV_Target
{
    float2 tc = saturate(i.texcoord) * texScale;
    float y = texY.Sample(sampLinear, tc);
    float2 uv = texUV.Sample(sampLinear, tc);

    y = 1.1685f * (y - (4096.0f / 65535.0f));
    float u = uv.x - 0.5f;
    float v = uv.y - 0.5f;

    float r = y + 1.79274107f * v;
    float g = y - 0.21324861f * u - 0.53290933f * v;
    float b = y + 2.11240179f * u;

    return float4(saturate(r), saturate(g), saturate(b), 1.0f);
}
)";

    bool compileHlsl(const char *source, const char *entryPoint, const char *target,
                     ComPtr<ID3DBlob> &outBlob)
    {
        outBlob.Reset();
        ComPtr<ID3DBlob> errorBlob;

        const HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entryPoint,
                                      target, 0, 0, &outBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) {
                qWarning() << QStringLiteral("[Nv12Render_D3d11va] HLSL compile failed:")
                           << reinterpret_cast<const char *>(errorBlob->GetBufferPointer());
            } else {
                qWarning() << QStringLiteral("[Nv12Render_D3d11va] HLSL compile failed, HRESULT:")
                           << Qt::hex << hr;
            }
            return false;
        }

        return true;
    }

    bool unregisterWglTexture(wgl::WglDeviceRef &device, HANDLE &handle)
    {
        if (!handle) {
            return true;
        }
        if (!device.isValid()) {
            handle = nullptr;
            return false;
        }

        device.wglDXUnlockObjectsNV(1, &handle);
        const bool ok = device.wglDXUnregisterObjectNV(handle);
        handle = nullptr;
        return ok;
    }

    bool tryGetFrameTextureAndSlice(const decoder_sdk::Frame &frame, ID3D11Texture2D *&outTexture,
                                    UINT &outSlice)
    {
        outTexture = nullptr;
        outSlice = 0;

        const auto curPixelForamt = frame.pixelFormat();
        if (curPixelForamt == decoder_sdk::ImageFormat::kD3d11va) {
            outTexture = reinterpret_cast<ID3D11Texture2D *>(frame.data(0));
            outSlice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame.data(1)));
            return outTexture != nullptr;
        }

#ifdef QSV_AVAILABLE
        if (curPixelForamt == decoder_sdk::ImageFormat::kQsv) {
            auto *const mfxSurface = reinterpret_cast<mfxFrameSurface1 *>(frame.data(3));
            if (!mfxSurface) {
                return false;
            }

            auto *const pair = reinterpret_cast<mfxHDLPair *>(mfxSurface->Data.MemId);
            if (!pair) {
                return false;
            }

            if (pair->first) {
                outTexture = reinterpret_cast<ID3D11Texture2D *>(pair->first);
            }
            if (pair->second) {
                outSlice = *reinterpret_cast<UINT *>(pair->second);
            }

            return outTexture != nullptr;
        }
#endif

#ifdef AMF_AVAILABLE
        if (curPixelForamt == decoder_sdk::ImageFormat::kAmf) {
            auto *const amfSurface = reinterpret_cast<amf::AMFSurface *>(frame.data(0));
            if (!amfSurface) {
                return false;
            }
            outTexture = amf_utils::getPackedSurfaceDX11(amfSurface);
            outSlice = 0;
            return outTexture != nullptr;
        }
#endif

        return false;
    }
} // namespace

Nv12Render_D3d11va::Nv12Render_D3d11va()
    : VideoRender()
{
    initializeD3DResource(d3d11_utils::getD3D11Device(), d3d11_utils::getWglDeviceRef());
}

Nv12Render_D3d11va::~Nv12Render_D3d11va()
{
    cleanup();

    d3d11Context_.Reset();
    d3d11DeferredContext_.Reset();
    d3d11Device_.Reset();
}

bool Nv12Render_D3d11va::shouldRebuild() const
{
    return shouldReBuild_;
}

QString Nv12Render_D3d11va::renderName() const
{
    return QStringLiteral("D3D11VA Interop To OpenGL Render");
}

bool Nv12Render_D3d11va::initRenderVbo(const bool horizontal, const bool vertical)
{
    horizontalMirror_ = horizontal;
    verticalMirror_ = vertical;
    return true;
}

bool Nv12Render_D3d11va::initRenderShader(const decoder_sdk::Frame &frame)
{
    return true;
}

bool Nv12Render_D3d11va::initRenderTexture(const decoder_sdk::Frame &frame)
{
    if (!createRGBTexture())
        return false;

    return true;
}

bool Nv12Render_D3d11va::initInteropsResource(const decoder_sdk::Frame &frame)
{
    ID3D11Texture2D *sourceTexture = nullptr;
    UINT arraySlice = 0;
    tryGetFrameTextureAndSlice(frame, sourceTexture, arraySlice);

    if (!sourceTexture) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Invalid D3D11 source texture!");
        return false;
    }

    ComPtr<ID3D11Device> textureDevice;
    sourceTexture->GetDevice(&textureDevice);
    if (textureDevice.Get() != d3d11Device_.Get()) {
        qInfo() << QStringLiteral(
            "[Nv12Render_D3d11va] The decoding-side D3D11 Device is inconsistent with the one "
            "currently in use; therefore, the D3D11 Resource needs to be reinitialized.");
        if (!initializeD3DResource(textureDevice)) {
            qWarning() << QStringLiteral(
                "[Nv12Render_D3d11va] Reinitialize D3D11 resource failed!");
            return false;
        }
    }

    if (!checkWGLInterop()) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Failed to initialize WGL interop!");
        return false;
    }

    if (!initializeNv12Converter()) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Failed to initialize NV12 converter!");
        return false;
    }

    if (!ensureOutputTextureAndInterop(frame.width(), frame.height())) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Failed to initialize interop output!");
        return false;
    }

    shouldReBuild_ = false;
    return true;
}

bool Nv12Render_D3d11va::renderFrame(const decoder_sdk::Frame &frame)
{
    if (!frame.isValid())
        return false;

    if (!processNV12ToRGB(frame)) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Failed to process NV12 to RGB!");
        return false;
    }

    return blitToCurrentFbo(frame.width(), frame.height());
}

bool Nv12Render_D3d11va::initializeD3DResource(const ComPtr<ID3D11Device> &d3d11Device,
                                               const wgl::WglDeviceRef &wglDevice)
{
    d3d11Device5_.Reset();
    d3d11Context4_.Reset();
    executeFinishedFence_.Reset();
    executeFinishedQuery_.Reset();
    if (executeFenceEvent_) {
        CloseHandle(executeFenceEvent_);
        executeFenceEvent_ = nullptr;
    }
    executeFenceValue_ = 0;
    useFenceSync_ = false;

    d3d11Device_ = d3d11Device;

    if (!d3d11Device_.Get()) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Can not get D3D11 Device!");
        return false;
    }

    d3d11Device_->GetImmediateContext(&d3d11Context_);
    if (!d3d11Context_.Get()) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Can not get D3D11 Context!");
        return false;
    }

    d3d11DeferredContext_.Reset();
    HRESULT hr = d3d11Device_->CreateDeferredContext(0, &d3d11DeferredContext_);
    if (FAILED(hr) || !d3d11DeferredContext_) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] CreateDeferredContext failed, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    // 如果传入的wgl设备有效，就使用传入的设备，否则使用d3d11 device新建
    if (wglDevice.isValid()) {
        wglD3DDevice_ = wglDevice;
    } else {
        wglD3DDevice_ = wgl::WglDeviceRef(d3d11Device_.Get());
    }

    if (!wglD3DDevice_.isValid()) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Can not get wgl Device!");
        return false;
    }
    return true;
}

bool Nv12Render_D3d11va::checkWGLInterop()
{
    if (!d3d11Device_) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] D3D11 device is null!");
        return false;
    }

    if (!wglD3DDevice_.isValid()) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] WGL device is invalid!");
        return false;
    }

    return true;
}

bool Nv12Render_D3d11va::initializeNv12Converter()
{
    if (!d3d11Context_) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] D3D11 context is invalid!");
        return false;
    }

    const bool syncReady =
        useFenceSync_ ? (d3d11Device5_ && d3d11Context4_ && executeFinishedFence_ && executeFenceEvent_)
                      : (executeFinishedQuery_ != nullptr);
    if (nv12VertexShader_ && nv12PixelShader_ && p010PixelShader_ && nv12Sampler_ && texScaleCb_ &&
        syncReady) {
        return true;
    }

    ComPtr<ID3DBlob> vsBlob;
    if (!compileHlsl(nv12VsHlsl, "main", "vs_4_0", vsBlob)) {
        return false;
    }

    HRESULT hr = d3d11Device_->CreateVertexShader(
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &nv12VertexShader_);
    if (FAILED(hr)) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] CreateVertexShader failed, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    ComPtr<ID3DBlob> psBlob;
    if (!compileHlsl(nv12PsHlsl, "main", "ps_4_0", psBlob)) {
        return false;
    }

    hr = d3d11Device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                         nullptr, &nv12PixelShader_);
    if (FAILED(hr)) {
        qWarning() << QStringLiteral(
                          "[Nv12Render_D3d11va] Create NV12 pixel shader failed, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    psBlob.Reset();
    if (!compileHlsl(p010PsHlsl, "main", "ps_4_0", psBlob)) {
        return false;
    }

    hr = d3d11Device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                         nullptr, &p010PixelShader_);
    if (FAILED(hr)) {
        qWarning() << QStringLiteral(
                          "[Nv12Render_D3d11va] Create P010 pixel shader failed, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = d3d11Device_->CreateSamplerState(&samplerDesc, &nv12Sampler_);
    if (FAILED(hr)) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] CreateSamplerState failed, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = 16;
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = 0;
    cbDesc.MiscFlags = 0;

    hr = d3d11Device_->CreateBuffer(&cbDesc, nullptr, &texScaleCb_);
    if (FAILED(hr) || !texScaleCb_) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Create constant buffer failed, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    d3d11Device5_.Reset();
    d3d11Context4_.Reset();
    executeFinishedFence_.Reset();
    executeFinishedQuery_.Reset();
    if (executeFenceEvent_) {
        CloseHandle(executeFenceEvent_);
        executeFenceEvent_ = nullptr;
    }
    executeFenceValue_ = 0;
    useFenceSync_ = false;

    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> fence;
    HANDLE fenceEvent = nullptr;
    bool fenceReady = false;

    hr = d3d11Device_.As(&device5);
    if (SUCCEEDED(hr) && device5) {
        hr = d3d11Context_.As(&context4);
        if (SUCCEEDED(hr) && context4) {
            hr = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence),
                                      reinterpret_cast<void **>(fence.GetAddressOf()));
            if (SUCCEEDED(hr) && fence) {
                fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (fenceEvent) {
                    d3d11Device5_ = device5;
                    d3d11Context4_ = context4;
                    executeFinishedFence_ = fence;
                    executeFenceEvent_ = fenceEvent;
                    useFenceSync_ = true;
                    fenceReady = true;
                    qDebug() << QStringLiteral(
                        "[Nv12Render_D3d11va] Synchronization using fence is ready.");
                }
            }
        }
    }

    if (!fenceReady) {
        if (fenceEvent) {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }

        D3D11_QUERY_DESC queryDesc = {};
        queryDesc.Query = D3D11_QUERY_EVENT;
        queryDesc.MiscFlags = 0;
        hr = d3d11Device_->CreateQuery(&queryDesc, &executeFinishedQuery_);
        if (FAILED(hr) || !executeFinishedQuery_) {
            qWarning() << QStringLiteral(
                              "[Nv12Render_D3d11va] Neither fence nor query sync is available, HRESULT:")
                       << Qt::hex << hr;
            return false;
        }
    }

    return true;
}

void Nv12Render_D3d11va::cleanup()
{
    if (!unregisterWglTexture(wglD3DDevice_, wglTextureHandle_) && wglD3DDevice_.isValid()) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Failed to unregister WGL object!");
    }

    if (glRGBTexture_) {
        glDeleteTextures(1, &glRGBTexture_);
        glRGBTexture_ = 0;
    }

    if (glInteropReadFbo_) {
        glDeleteFramebuffers(1, &glInteropReadFbo_);
        glInteropReadFbo_ = 0;
    }

    inputCopyYSrv_.Reset();
    inputCopyUVSrv_.Reset();
    inputCopyTexture_.Reset();
    executeFinishedFence_.Reset();
    executeFinishedQuery_.Reset();
    d3d11Context4_.Reset();
    d3d11Device5_.Reset();
    if (executeFenceEvent_) {
        CloseHandle(executeFenceEvent_);
        executeFenceEvent_ = nullptr;
    }
    executeFenceValue_ = 0;
    useFenceSync_ = false;

    outputWidth_ = 0;
    outputHeight_ = 0;

    outputRTV_.Reset();
    outputRGBTexture_.Reset();
}

bool Nv12Render_D3d11va::createRGBTexture()
{
    if (glRGBTexture_) {
        glDeleteTextures(1, &glRGBTexture_);
        glRGBTexture_ = 0;
    }

    // 创建OpenGL纹理
    glGenTextures(1, &glRGBTexture_);
    glBindTexture(GL_TEXTURE_2D, glRGBTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

bool Nv12Render_D3d11va::ensureInteropReadFbo()
{
    if (!glRGBTexture_) {
        return false;
    }

    if (!glInteropReadFbo_) {
        glGenFramebuffers(1, &glInteropReadFbo_);
        if (!glInteropReadFbo_) {
            return false;
        }
    }

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    glBindFramebuffer(GL_FRAMEBUFFER, glInteropReadFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glRGBTexture_, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    return status == GL_FRAMEBUFFER_COMPLETE;
}

bool Nv12Render_D3d11va::blitToCurrentFbo(int width, int height)
{
    if (!ensureInteropReadFbo()) {
        return false;
    }

    GLint prevReadFbo = 0;
    GLint prevDrawFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, glInteropReadFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDrawFbo));

    const int srcX0 = horizontalMirror_ ? width : 0;
    const int srcX1 = horizontalMirror_ ? 0 : width;
    const int srcY0 = verticalMirror_ ? 0 : height;
    const int srcY1 = verticalMirror_ ? height : 0;

    glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, 0, 0, width, height, GL_COLOR_BUFFER_BIT,
                      GL_LINEAR);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevReadFbo));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDrawFbo));
    return true;
}

bool Nv12Render_D3d11va::ensureOutputTextureAndInterop(int width, int height)
{
    if (!d3d11Device_ || !d3d11Context_) {
        return false;
    }

    if (!glRGBTexture_) {
        return false;
    }

    if (outputRGBTexture_ && outputRTV_ && wglTextureHandle_ && outputWidth_ == width &&
        outputHeight_ == height) {
        return true;
    }

    unregisterWglTexture(wglD3DDevice_, wglTextureHandle_);

    outputRTV_.Reset();
    outputRGBTexture_.Reset();

    outputWidth_ = 0;
    outputHeight_ = 0;

    if (!registerTextureWithOpenGL(width, height)) {
        return false;
    }

    outputWidth_ = width;
    outputHeight_ = height;
    return true;
}

bool Nv12Render_D3d11va::ensureInputShaderResources(ID3D11DeviceContext *cmdContext,
                                                    ID3D11Texture2D *sourceTexture, UINT arraySlice,
                                                    ComPtr<ID3D11ShaderResourceView> &outYSrv,
                                                    ComPtr<ID3D11ShaderResourceView> &outUVSrv)
{
    outYSrv.Reset();
    outUVSrv.Reset();

    if (!cmdContext) {
        cmdContext = d3d11Context_.Get();
    }

    if (!sourceTexture || !d3d11Device_ || !cmdContext) {
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    sourceTexture->GetDesc(&sourceDesc);

    if (sourceDesc.Format != DXGI_FORMAT_NV12 && sourceDesc.Format != DXGI_FORMAT_P010) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Unsupported input DXGI_FORMAT:")
                   << static_cast<int>(sourceDesc.Format);
        return false;
    }

    ComPtr<ID3D11Device3> device3;
    d3d11Device_.As(&device3);

    auto createSrvsForTexture = [&](ID3D11Texture2D *texture, UINT slice,
                                    ComPtr<ID3D11ShaderResourceView> &ySrv,
                                    ComPtr<ID3D11ShaderResourceView> &uvSrv) -> HRESULT {
        ySrv.Reset();
        uvSrv.Reset();

        D3D11_TEXTURE2D_DESC desc = {};
        texture->GetDesc(&desc);

        const DXGI_FORMAT yFmt =
            (desc.Format == DXGI_FORMAT_P010) ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        const DXGI_FORMAT uvFmt =
            (desc.Format == DXGI_FORMAT_P010) ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;

        if (device3) {
            ComPtr<ID3D11ShaderResourceView1> ySrv1;
            ComPtr<ID3D11ShaderResourceView1> uvSrv1;

            D3D11_SHADER_RESOURCE_VIEW_DESC1 yDesc1 = {};
            yDesc1.Format = yFmt;
            if (desc.ArraySize > 1) {
                yDesc1.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                yDesc1.Texture2DArray.MostDetailedMip = 0;
                yDesc1.Texture2DArray.MipLevels = 1;
                yDesc1.Texture2DArray.FirstArraySlice = slice;
                yDesc1.Texture2DArray.ArraySize = 1;
                yDesc1.Texture2DArray.PlaneSlice = 0;
            } else {
                yDesc1.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                yDesc1.Texture2D.MostDetailedMip = 0;
                yDesc1.Texture2D.MipLevels = 1;
                yDesc1.Texture2D.PlaneSlice = 0;
            }

            HRESULT hr = device3->CreateShaderResourceView1(texture, &yDesc1, &ySrv1);
            if (FAILED(hr)) {
                return hr;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC1 uvDesc1 = {};
            uvDesc1.Format = uvFmt;
            if (desc.ArraySize > 1) {
                uvDesc1.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                uvDesc1.Texture2DArray.MostDetailedMip = 0;
                uvDesc1.Texture2DArray.MipLevels = 1;
                uvDesc1.Texture2DArray.FirstArraySlice = slice;
                uvDesc1.Texture2DArray.ArraySize = 1;
                uvDesc1.Texture2DArray.PlaneSlice = 1;
            } else {
                uvDesc1.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                uvDesc1.Texture2D.MostDetailedMip = 0;
                uvDesc1.Texture2D.MipLevels = 1;
                uvDesc1.Texture2D.PlaneSlice = 1;
            }

            hr = device3->CreateShaderResourceView1(texture, &uvDesc1, &uvSrv1);
            if (FAILED(hr)) {
                return hr;
            }

            ySrv = ySrv1;
            uvSrv = uvSrv1;
            return S_OK;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC yDesc = {};
        yDesc.Format = yFmt;
        if (desc.ArraySize > 1) {
            yDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            yDesc.Texture2DArray.MostDetailedMip = 0;
            yDesc.Texture2DArray.MipLevels = 1;
            yDesc.Texture2DArray.FirstArraySlice = slice;
            yDesc.Texture2DArray.ArraySize = 1;
        } else {
            yDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            yDesc.Texture2D.MostDetailedMip = 0;
            yDesc.Texture2D.MipLevels = 1;
        }

        HRESULT hr = d3d11Device_->CreateShaderResourceView(texture, &yDesc, &ySrv);
        if (FAILED(hr)) {
            return hr;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC uvDesc = {};
        uvDesc.Format = uvFmt;
        if (desc.ArraySize > 1) {
            uvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            uvDesc.Texture2DArray.MostDetailedMip = 0;
            uvDesc.Texture2DArray.MipLevels = 1;
            uvDesc.Texture2DArray.FirstArraySlice = slice;
            uvDesc.Texture2DArray.ArraySize = 1;
        } else {
            uvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            uvDesc.Texture2D.MostDetailedMip = 0;
            uvDesc.Texture2D.MipLevels = 1;
        }

        return d3d11Device_->CreateShaderResourceView(texture, &uvDesc, &uvSrv);
    };

    HRESULT hr = createSrvsForTexture(sourceTexture, arraySlice, outYSrv, outUVSrv);
    if (SUCCEEDED(hr) && outYSrv && outUVSrv) {
        return true;
    }

    const D3D11_TEXTURE2D_DESC expectedCopyDesc = [&]() {
        D3D11_TEXTURE2D_DESC copyDesc = sourceDesc;
        copyDesc.MipLevels = 1;
        copyDesc.ArraySize = 1;
        copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copyDesc.CPUAccessFlags = 0;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.MiscFlags = 0;
        return copyDesc;
    }();

    auto recreateCopyTexture = [&]() -> bool {
        inputCopyYSrv_.Reset();
        inputCopyUVSrv_.Reset();
        inputCopyTexture_.Reset();

        hr = d3d11Device_->CreateTexture2D(&expectedCopyDesc, nullptr, &inputCopyTexture_);
        if (FAILED(hr) || !inputCopyTexture_) {
            qWarning()
                << QStringLiteral("[Nv12Render_D3d11va] Create input copy texture failed, HRESULT:")
                << Qt::hex << hr;
            return false;
        }

        hr = createSrvsForTexture(inputCopyTexture_.Get(), 0, inputCopyYSrv_, inputCopyUVSrv_);
        if (FAILED(hr) || !inputCopyYSrv_ || !inputCopyUVSrv_) {
            qWarning() << QStringLiteral(
                              "[Nv12Render_D3d11va] Create input copy SRV failed, HRESULT:")
                       << Qt::hex << hr;
            return false;
        }

        return true;
    };

    if (!inputCopyTexture_ || !inputCopyYSrv_ || !inputCopyUVSrv_) {
        if (!recreateCopyTexture()) {
            return false;
        }
    } else {
        D3D11_TEXTURE2D_DESC currentCopyDesc = {};
        inputCopyTexture_->GetDesc(&currentCopyDesc);
        if (currentCopyDesc.Width != expectedCopyDesc.Width ||
            currentCopyDesc.Height != expectedCopyDesc.Height ||
            currentCopyDesc.Format != expectedCopyDesc.Format) {
            if (!recreateCopyTexture()) {
                return false;
            }
        }
    }

    const UINT srcSubresource = D3D11CalcSubresource(0, arraySlice, 1);
    cmdContext->CopySubresourceRegion(inputCopyTexture_.Get(), 0, 0, 0, 0, sourceTexture,
                                      srcSubresource, nullptr);

    outYSrv = inputCopyYSrv_;
    outUVSrv = inputCopyUVSrv_;
    return true;
}

bool Nv12Render_D3d11va::processNV12ToRGB(const decoder_sdk::Frame &frame)
{
    ID3D11Texture2D *sourceTexture = nullptr;
    UINT arraySlice = 0;
    tryGetFrameTextureAndSlice(frame, sourceTexture, arraySlice);

    if (!sourceTexture || !nv12VertexShader_ || !nv12PixelShader_ || !p010PixelShader_ ||
        !nv12Sampler_) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Missing required resources!");
        return false;
    }

    ComPtr<ID3D11Device> sourceDevice;
    sourceTexture->GetDevice(&sourceDevice);
    if (sourceDevice.Get() != d3d11Device_.Get()) {
        shouldReBuild_ = true;
        qWarning() << QStringLiteral(
            "[Nv12Render_D3d11va] The decoding-side D3D11 Device is inconsistent with the one "
            "currently in use; therefore, the render needs to be rebuild.");
        return false;
    }

    if (!ensureOutputTextureAndInterop(frame.width(), frame.height())) {
        shouldReBuild_ = true;
        return false;
    }

    if (!wglTextureHandle_ || !outputRTV_) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Missing output interop resources!");
        return false;
    }

    ComPtr<ID3D11ShaderResourceView> ySrv;
    ComPtr<ID3D11ShaderResourceView> uvSrv;
    if (!d3d11DeferredContext_) {
        HRESULT hr = d3d11Device_->CreateDeferredContext(0, &d3d11DeferredContext_);
        if (FAILED(hr) || !d3d11DeferredContext_) {
            qWarning()
                << QStringLiteral("[Nv12Render_D3d11va] CreateDeferredContext failed, HRESULT:")
                << Qt::hex << hr;
            return false;
        }
    }

    if (!ensureInputShaderResources(d3d11DeferredContext_.Get(), sourceTexture, arraySlice, ySrv,
                                    uvSrv)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    sourceTexture->GetDesc(&sourceDesc);

    d3d11DeferredContext_->ClearState();

    struct TexScaleCBData {
        float scaleX;
        float scaleY;
        float pad0;
        float pad1;
    } cbData = {};

    cbData.scaleX = (sourceDesc.Width > 0) ? (static_cast<float>(frame.width()) /
                                              static_cast<float>(sourceDesc.Width))
                                           : 1.0f;
    cbData.scaleY = (sourceDesc.Height > 0) ? (static_cast<float>(frame.height()) /
                                               static_cast<float>(sourceDesc.Height))
                                            : 1.0f;
    if (cbData.scaleX > 1.0f) {
        cbData.scaleX = 1.0f;
    }
    if (cbData.scaleY > 1.0f) {
        cbData.scaleY = 1.0f;
    }

    if (texScaleCb_) {
        d3d11DeferredContext_->UpdateSubresource(texScaleCb_.Get(), 0, nullptr, &cbData, 0, 0);
        ID3D11Buffer *cb = texScaleCb_.Get();
        d3d11DeferredContext_->PSSetConstantBuffers(0, 1, &cb);
    }

    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(outputWidth_);
    viewport.Height = static_cast<float>(outputHeight_);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    d3d11DeferredContext_->RSSetViewports(1, &viewport);

    ID3D11RenderTargetView *rtv = outputRTV_.Get();
    d3d11DeferredContext_->OMSetRenderTargets(1, &rtv, nullptr);

    d3d11DeferredContext_->IASetInputLayout(nullptr);
    d3d11DeferredContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    d3d11DeferredContext_->VSSetShader(nv12VertexShader_.Get(), nullptr, 0);
    if (sourceDesc.Format == DXGI_FORMAT_P010) {
        d3d11DeferredContext_->PSSetShader(p010PixelShader_.Get(), nullptr, 0);
    } else {
        d3d11DeferredContext_->PSSetShader(nv12PixelShader_.Get(), nullptr, 0);
    }

    ID3D11SamplerState *sampler = nv12Sampler_.Get();
    d3d11DeferredContext_->PSSetSamplers(0, 1, &sampler);

    ID3D11ShaderResourceView *srvs[2] = { ySrv.Get(), uvSrv.Get() };
    d3d11DeferredContext_->PSSetShaderResources(0, 2, srvs);

    d3d11DeferredContext_->Draw(3, 0);

    ID3D11ShaderResourceView *nullSrvs[2] = { nullptr, nullptr };
    d3d11DeferredContext_->PSSetShaderResources(0, 2, nullSrvs);

    ComPtr<ID3D11CommandList> commandList;
    HRESULT finishHr = d3d11DeferredContext_->FinishCommandList(FALSE, &commandList);
    if (FAILED(finishHr) || !commandList) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] FinishCommandList failed, HRESULT:")
                   << Qt::hex << finishHr;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_d3d11ImmediateExecMutex);
        wglD3DDevice_.wglDXUnlockObjectsNV(1, &wglTextureHandle_);
        d3d11Context_->ExecuteCommandList(commandList.Get(), TRUE);
        if (!waitForExecuteComplete()) {
            qWarning() << QStringLiteral("[Nv12Render_D3d11va] Wait execute complete failed!");
            wglD3DDevice_.wglDXLockObjectsNV(1, &wglTextureHandle_);
            return false;
        }
        wglD3DDevice_.wglDXLockObjectsNV(1, &wglTextureHandle_);
    }

    return true;
}

bool Nv12Render_D3d11va::registerTextureWithOpenGL(int width, int height)
{
    // 创建输出RGB纹理
    D3D11_TEXTURE2D_DESC rgbDesc = {};
    rgbDesc.Width = width;
    rgbDesc.Height = height;
    rgbDesc.MipLevels = 1;
    rgbDesc.ArraySize = 1;
    rgbDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rgbDesc.SampleDesc.Count = 1;
    rgbDesc.SampleDesc.Quality = 0;
    rgbDesc.Usage = D3D11_USAGE_DEFAULT;
    rgbDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    rgbDesc.CPUAccessFlags = 0;
    rgbDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr = d3d11Device_->CreateTexture2D(&rgbDesc, nullptr, &outputRGBTexture_);
    if (FAILED(hr)) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Failed to create RGB texture, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    hr = d3d11Device_->CreateRenderTargetView(outputRGBTexture_.Get(), nullptr, &outputRTV_);
    if (FAILED(hr)) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Failed to create output RTV, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    if (!wglD3DDevice_.isValid() || !outputRGBTexture_) {
        qWarning() << QStringLiteral(
            "[Nv12Render_D3d11va] Missing resources for OpenGL registration!");
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, glRGBTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    // 注册D3D纹理到WGL
    wglTextureHandle_ = wglD3DDevice_.wglDXRegisterObjectNV(outputRGBTexture_.Get(), glRGBTexture_,
                                                            GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
    if (!wglTextureHandle_) {
        DWORD error = GetLastError();
        qWarning() << QStringLiteral(
                          "[Nv12Render_D3d11va] Failed to register RGB texture with WGL, error:")
                   << error;
        return false;
    }
    wglD3DDevice_.wglDXLockObjectsNV(1, &wglTextureHandle_);

    return true;
}

bool Nv12Render_D3d11va::waitForExecuteComplete()
{
    if (useFenceSync_) {
        if (!d3d11Context4_ || !executeFinishedFence_ || !executeFenceEvent_) {
            return false;
        }

        ++executeFenceValue_;
        HRESULT hr = d3d11Context4_->Signal(executeFinishedFence_.Get(), executeFenceValue_);
        if (FAILED(hr)) {
            return false;
        }

        if (executeFinishedFence_->GetCompletedValue() >= executeFenceValue_) {
            return true;
        }

        hr = executeFinishedFence_->SetEventOnCompletion(executeFenceValue_, executeFenceEvent_);
        if (FAILED(hr)) {
            return false;
        }

        return WaitForSingleObject(executeFenceEvent_, INFINITE) == WAIT_OBJECT_0;
    }

    if (!d3d11Context_ || !executeFinishedQuery_) {
        return false;
    }

    d3d11Context_->End(executeFinishedQuery_.Get());
    for (int i = 0; i < 20000; ++i) {
        const HRESULT hr =
            d3d11Context_->GetData(executeFinishedQuery_.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) {
            return true;
        }
        if (hr != S_FALSE) {
            return false;
        }
        if ((i % 100) == 0) {
            Sleep(1);
        } else {
            Sleep(0);
        }
    }

    return false;
}

#endif
