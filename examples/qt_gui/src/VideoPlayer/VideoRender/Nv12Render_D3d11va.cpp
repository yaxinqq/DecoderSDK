#ifdef D3D11VA_AVAILABLE
#include "Nv12Render_D3d11va.h"
#include "Commonutils.h"

#ifdef _WIN32
#include <Windows.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#endif

#include <QDebug>
#include <iostream>

namespace {
const char *vsrc = R"(
#ifdef GL_ES
    precision mediump float;
#endif

    attribute vec4 vertexIn;
    attribute vec2 textureIn;
    varying vec2 textureOut;
    void main(void)
    {
        gl_Position = vertexIn;
        textureOut = textureIn;
    }
)";

const char *fsrc = R"(
#ifdef GL_ES
    precision mediump float;
#endif

    uniform sampler2D rgbTexture;
    varying vec2 textureOut;
    void main(void)
    {
        gl_FragColor = texture2D(rgbTexture, textureOut);
    }
)";
} // namespace

Nv12Render_D3d11va::Nv12Render_D3d11va()
    : VideoRender() 
    , d3d11Device_(D3D11Utils::getD3D11Device())
{
    if (d3d11Device_) {
        d3d11Device_->GetImmediateContext(&d3d11Context_);
    }
}

Nv12Render_D3d11va::~Nv12Render_D3d11va()
{
    cleanup();

    if (wglD3DDevice_) {
        wglDXCloseDeviceNV(wglD3DDevice_);
        wglD3DDevice_ = nullptr;
    }

    if (videoProcessor_)
        videoProcessor_.Reset();
    if (videoProcessorEnum_)
        videoProcessorEnum_.Reset();
    if (videoContext_)
        videoContext_.Reset();
    if (videoDevice_)
        videoDevice_.Reset();

    vbo_.destroy();

    if (d3d11Context_)
        d3d11Context_.Reset();
}

bool Nv12Render_D3d11va::initRenderVbo(const bool horizontal, const bool vertical)
{
    initDefaultVBO(vbo_, horizontal, vertical);
    return true;
}

bool Nv12Render_D3d11va::initRenderShader(const decoder_sdk::Frame &frame)
{
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vsrc);
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fsrc);
    program_.link();
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
    if (!initializeWGLInterop()) {
        qWarning() << "[Nv12Render_D3d11va] Failed to initialize WGL interop!";
        return false;
    }

    if (!initializeVideoProcessor(frame.width(), frame.height())) {
        qWarning() << "[Nv12Render_D3d11va] Failed to initialize VideoProcessor!";
        return false;
    }

    if (!registerTextureWithOpenGL(frame.width(), frame.height())) {
        qWarning() << "[Nv12Render_D3d11va] Failed to register D3D texture to OpenGL texture!";
        return false;
    }

    return true;
}

bool Nv12Render_D3d11va::renderFrame(const decoder_sdk::Frame &frame)
{
    if (!frame.isValid())
        return false;

    if (!processNV12ToRGB(frame)) {
        qWarning() << "[Nv12Render_D3d11va] Failed to process NV12 to RGB!";
        return false;
    }

    return drawFrame(glRGBTexture_);
}

bool Nv12Render_D3d11va::initializeWGLInterop()
{
    if (!loadWGLExtensions()) {
        qWarning() << "[Nv12Render_D3d11va] Failed to load WGL extensions!";
        return false;
    }

    if (!d3d11Device_) {
        qWarning() << "[Nv12Render_D3d11va] D3D11 device is null!";
        return false;
    }

    HRESULT hr = d3d11Device_->GetDeviceRemovedReason();
    if (FAILED(hr)) {
        qWarning() << "[Nv12Render_D3d11va] D3D11 device is in error state, HRESULT:" << hr;
        return false;
    }

    wglD3DDevice_ = wglDXOpenDeviceNV(d3d11Device_.Get());
    if (!wglD3DDevice_) {
        DWORD error = GetLastError();
        qWarning() << "[Nv12Render_D3d11va] Failed to open D3D device for WGL interop, error:" << error;
        return false;
    }

    return true;
}

bool Nv12Render_D3d11va::initializeVideoProcessor(int width, int height)
{
    if (!d3d11Context_) {
        qWarning() << "[Nv12Render_D3d11va] D3D11 context is invalid!";
        return false;
    }

    // 获取VideoDevice接口
    HRESULT hr = d3d11Device_->QueryInterface(__uuidof(ID3D11VideoDevice), (void **)&videoDevice_);
    if (FAILED(hr)) {
        qWarning() << "[Nv12Render_D3d11va] Failed to get VideoDevice interface, HRESULT:" << hr;
        return false;
    }

    // 获取VideoContext接口
    hr = d3d11Context_->QueryInterface(__uuidof(ID3D11VideoContext), (void **)&videoContext_);
    if (FAILED(hr)) {
        qWarning() << "[Nv12Render_D3d11va] Failed to get VideoContext interface, HRESULT:" << hr;
        return false;
    }

    // 创建VideoProcessorEnumerator
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = width;
    contentDesc.InputHeight = height;
    contentDesc.OutputWidth = width;
    contentDesc.OutputHeight = height;
    contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = videoDevice_->CreateVideoProcessorEnumerator(&contentDesc, &videoProcessorEnum_);
    if (FAILED(hr)) {
        qWarning() << "Failed to create VideoProcessorEnumerator, HRESULT:" << hr;
        return false;
    }

    // 创建VideoProcessor
    hr = videoDevice_->CreateVideoProcessor(videoProcessorEnum_.Get(), 0, &videoProcessor_);
    if (FAILED(hr)) {
        qWarning() << "[Nv12Render_D3d11va] Failed to create VideoProcessor, HRESULT:" << hr;
        return false;
    }

    return true;
}

void Nv12Render_D3d11va::cleanup()
{
    if (wglTextureHandle_ && wglD3DDevice_) {
        wglDXUnregisterObjectNV(wglD3DDevice_, wglTextureHandle_);
        wglTextureHandle_ = nullptr;
    }

    if (glRGBTexture_) {
        glDeleteTextures(1, &glRGBTexture_);
        glRGBTexture_ = 0;
    }

    // 清理VideoProcessor资源
    if (inputView_)
        inputView_.Reset();
    if (outputView_)
        outputView_.Reset();

    // 清理纹理
    if (inputNV12Texture_)
        inputNV12Texture_.Reset();
    if (outputRGBTexture_)
        outputRGBTexture_.Reset();
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

bool Nv12Render_D3d11va::createRGBTexture()
{
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

bool Nv12Render_D3d11va::processNV12ToRGB(const decoder_sdk::Frame &frame)
{
    ID3D11Texture2D *sourceTexture = reinterpret_cast<ID3D11Texture2D *>(frame.data(0));
    if (!sourceTexture || !videoProcessor_ || !videoContext_) {
        qWarning() << "[Nv12Render_D3d11va] Missing required resources!";
        return false;
    }

    // 获取源纹理的共享句柄并在当前设备上打开
    ComPtr<IDXGIResource> dxgiResource;
    HRESULT hr = sourceTexture->QueryInterface(__uuidof(IDXGIResource), (void **)&dxgiResource);
    if (FAILED(hr)) {
        qWarning() << "[Nv12Render_D3d11va] Failed to query source DXGI resource, HRESULT:" << hr;
        return false;
    }

    HANDLE sharedHandle = nullptr;
    hr = dxgiResource->GetSharedHandle(&sharedHandle);
    if (FAILED(hr)) {
        qWarning() << "[Nv12Render_D3d11va] Failed to get source shared handle, HRESULT:" << hr;
        return false;
    }

    ComPtr<ID3D11Texture2D> sharedSourceTexture;
    hr = d3d11Device_->OpenSharedResource(sharedHandle, __uuidof(ID3D11Texture2D),
                                          (void **)&sharedSourceTexture);
    if (FAILED(hr)) {
        qWarning() << "[Nv12Render_D3d11va] Failed to open shared source texture, HRESULT:" << hr;
        return false;
    }

    // 创建或更新InputView
    if (!inputView_ || inputNV12Texture_.Get() != sharedSourceTexture.Get()) {
        inputNV12Texture_ = sharedSourceTexture;

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {};
        inputViewDesc.FourCC = 0;
        inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputViewDesc.Texture2D.MipSlice = 0;
        inputViewDesc.Texture2D.ArraySlice =
            static_cast<UINT>(reinterpret_cast<intptr_t>(frame.data(1)));

        hr = videoDevice_->CreateVideoProcessorInputView(
            inputNV12Texture_.Get(), videoProcessorEnum_.Get(), &inputViewDesc, &inputView_);
        if (FAILED(hr)) {
            qWarning() << "[Nv12Render_D3d11va] Failed to create VideoProcessorInputView, HRESULT:"
                       << hr;
            return false;
        }
    }

    // 设置颜色空间
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColorSpace = {};
    inputColorSpace.YCbCr_Matrix = 1; // BT.709
    inputColorSpace.YCbCr_xvYCC = 0;
    inputColorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace = {};
    outputColorSpace.RGB_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;

    videoContext_->VideoProcessorSetStreamColorSpace(videoProcessor_.Get(), 0, &inputColorSpace);
    videoContext_->VideoProcessorSetOutputColorSpace(videoProcessor_.Get(), &outputColorSpace);

    // 执行颜色空间转换
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView_.Get();

    // 锁定WGL对象
    if (!wglDXLockObjectsNV(wglD3DDevice_, 1, &wglTextureHandle_)) {
        qWarning() << "[Nv12Render_D3d11va] Failed to lock WGL objects!";
        return false;
    }

    hr = videoContext_->VideoProcessorBlt(videoProcessor_.Get(), outputView_.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        qWarning() << "[Nv12Render_D3d11va] VideoProcessorBlt failed, HRESULT:" << hr;
        return false;
    }

    // 解锁WGL对象
    if (!wglDXUnlockObjectsNV(wglD3DDevice_, 1, &wglTextureHandle_)) {
        qWarning() << "[Nv12Render_D3d11va] Failed to unlock WGL objects!";
        return false;
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
        qWarning() << "[Nv12Render_D3d11va] Failed to create RGB texture, HRESULT:" << hr;
        return false;
    }

    // 获取共享句柄
    ComPtr<IDXGIResource> dxgiResource;
    hr = outputRGBTexture_->QueryInterface(__uuidof(IDXGIResource), (void **)&dxgiResource);
    if (FAILED(hr)) {
        qWarning() << "[Nv12Render_D3d11va] Failed to query DXGI resource, HRESULT:" << hr;
        return false;
    }

    hr = dxgiResource->GetSharedHandle(&rgbSharedHandle_);
    if (FAILED(hr)) {
        qWarning() << "[Nv12Render_D3d11va] Failed to get shared handle, HRESULT:" << hr;
        return false;
    }

    // 创建VideoProcessorOutputView
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {};
    outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDesc.Texture2D.MipSlice = 0;

    hr = videoDevice_->CreateVideoProcessorOutputView(
        outputRGBTexture_.Get(), videoProcessorEnum_.Get(), &outputViewDesc, &outputView_);
    if (FAILED(hr)) {
        qWarning() << "[Nv12Render_D3d11va] Failed to create VideoProcessorOutputView, HRESULT:"
                   << hr;
        return false;
    }

    if (!wglD3DDevice_ || !outputRGBTexture_) {
        qWarning() << "[Nv12Render_D3d11va] Missing resources for OpenGL registration!";
        return false;
    }

    // 注册D3D纹理到WGL
    wglTextureHandle_ = wglDXRegisterObjectNV(wglD3DDevice_, outputRGBTexture_.Get(), glRGBTexture_,
                                              GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
    if (!wglTextureHandle_) {
        DWORD error = GetLastError();
        qWarning() << "[Nv12Render_D3d11va] Failed to register RGB texture with WGL, error:" << error;
        return false;
    }

    return true;
}

bool Nv12Render_D3d11va::drawFrame(GLuint id)
{
    // 资源未就绪
    if (!program_.isLinked() || !wglTextureHandle_) {
        qWarning() << "[Nv12Render_D3d11va] Not ready for drawing!";
        return false;
    }

    // 锁定WGL对象
    if (!wglDXLockObjectsNV(wglD3DDevice_, 1, &wglTextureHandle_)) {
        return false;
    }

    clearGL();

    program_.bind();
    vbo_.bind();

    // 绑定RGB纹理
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, id);
    program_.setUniformValue("rgbTexture", 0);

    // 设置顶点属性
    program_.enableAttributeArray("vertexIn");
    program_.enableAttributeArray("textureIn");
    program_.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 2, 0);
    program_.setAttributeBuffer("textureIn", GL_FLOAT, 2 * 4 * sizeof(GLfloat), 2, 0);

    // 绘制
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    program_.disableAttributeArray("vertexIn");
    program_.disableAttributeArray("textureIn");
    program_.release();

    // 解锁WGL对象
    if (!wglDXUnlockObjectsNV(wglD3DDevice_, 1, &wglTextureHandle_)) {
        return false;
    }

    return true;
}

#endif