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
// 简化的顶点着色器
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

// 简化的片段着色器 - 直接显示RGB纹理
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

Nv12Render_D3d11va::Nv12Render_D3d11va(ID3D11Device *d3d11Device)
    : d3d11Device_(d3d11Device ? d3d11Device : D3D11Utils::getD3D11Device())
{
    qDebug() << "Constructor called";

    if (!d3d11Device_) {
        qDebug() << "No D3D11 device provided, creating own";
        initializeD3D11();
        ownD3DDevice_ = true;
    }

    if (d3d11Device_) {
        d3d11Device_->GetImmediateContext(&d3d11Context_);

        // 获取D3D11.1接口
        d3d11Device_->QueryInterface(__uuidof(ID3D11Device1), (void **)&d3d11Device1_);
        d3d11Context_->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&d3d11Context1_);

        qDebug() << "D3D11 context obtained";
    }
}

Nv12Render_D3d11va::~Nv12Render_D3d11va()
{
    qDebug() << "Destructor called";
    cleanup();

    if (wglD3DDevice_) {
        qDebug() << "Closing WGL D3D device";
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

    if (d3d11Context1_)
        d3d11Context1_.Reset();
    if (d3d11Device1_)
        d3d11Device1_.Reset();
    if (d3d11Context_)
        d3d11Context_.Reset();

    if (ownD3DDevice_ && d3d11Device_) {
        d3d11Device_.Reset();
    }
}

void Nv12Render_D3d11va::initialize(const decoder_sdk::Frame &frame, const bool horizontal,
                                    const bool vertical)
{
    const auto width = frame.width();
    const auto height = frame.height();
    qDebug() << "Initialize called with size:" << width << "x" << height;

    initializeOpenGLFunctions();

    videoWidth_ = width;
    videoHeight_ = height;
    flipHorizontal_ = horizontal;
    flipVertical_ = vertical;

    if (!validateOpenGLContext()) {
        qDebug() << "Invalid OpenGL context";
        return;
    }

    if (!initializeWGLInterop()) {
        qDebug() << "Failed to initialize WGL interop";
        return;
    }

    if (!initializeVideoProcessor()) {
        qDebug() << "Failed to initialize VideoProcessor";
        return;
    }

    // 编译着色器
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vsrc);
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fsrc);
    program_.link();

    // 设置顶点数据
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
        // 纹理坐标
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

    if (!processNV12ToRGB(frame)) {
        qDebug() << "Failed to process NV12 to RGB";
        return;
    }
}

void Nv12Render_D3d11va::draw()
{
    if (!glRGBTexture_ || !program_.isLinked() || !wglTextureHandle_) {
        qDebug() << "Not ready for drawing";
        clearGL();
        return;
    }

    // 锁定WGL对象
    if (!wglDXLockObjectsNV(wglD3DDevice_, 1, &wglTextureHandle_)) {
        qDebug() << "Failed to lock WGL objects";
        clearGL();
        return;
    }

    program_.bind();
    vbo_.bind();

    // 绑定RGB纹理
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, glRGBTexture_);
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
        qDebug() << "Failed to unlock WGL objects";
    }
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

    if (!loadWGLExtensions()) {
        qDebug() << "Failed to load WGL extensions";
        return false;
    }

    if (!d3d11Device_) {
        qDebug() << "D3D11 device is null";
        return false;
    }

    HRESULT hr = d3d11Device_->GetDeviceRemovedReason();
    if (FAILED(hr)) {
        qDebug() << "D3D11 device is in error state, HRESULT:" << hr;
        return false;
    }

    wglD3DDevice_ = wglDXOpenDeviceNV(d3d11Device_.Get());
    if (!wglD3DDevice_) {
        DWORD error = GetLastError();
        qDebug() << "Failed to open D3D device for WGL interop, error:" << error;
        return false;
    }

    qDebug() << "WGL interop initialized successfully";
    return true;
}

bool Nv12Render_D3d11va::initializeVideoProcessor()
{
    qDebug() << "Initializing VideoProcessor";

    // 获取VideoDevice接口
    HRESULT hr = d3d11Device_->QueryInterface(__uuidof(ID3D11VideoDevice), (void **)&videoDevice_);
    if (FAILED(hr)) {
        qDebug() << "Failed to get VideoDevice interface";
        return false;
    }

    // 获取VideoContext接口
    hr = d3d11Context_->QueryInterface(__uuidof(ID3D11VideoContext), (void **)&videoContext_);
    if (FAILED(hr)) {
        qDebug() << "Failed to get VideoContext interface";
        return false;
    }

    // 创建VideoProcessorEnumerator
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = videoWidth_;
    contentDesc.InputHeight = videoHeight_;
    contentDesc.OutputWidth = videoWidth_;
    contentDesc.OutputHeight = videoHeight_;
    contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = videoDevice_->CreateVideoProcessorEnumerator(&contentDesc, &videoProcessorEnum_);
    if (FAILED(hr)) {
        qDebug() << "Failed to create VideoProcessorEnumerator";
        return false;
    }

    // 创建VideoProcessor
    hr = videoDevice_->CreateVideoProcessor(videoProcessorEnum_.Get(), 0, &videoProcessor_);
    if (FAILED(hr)) {
        qDebug() << "Failed to create VideoProcessor";
        return false;
    }

    qDebug() << "VideoProcessor initialized successfully";
    return true;
}

void Nv12Render_D3d11va::cleanup()
{
    qDebug() << "Cleaning up resources";

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

bool Nv12Render_D3d11va::createRGBTexture(int width, int height)
{
    // 创建输出RGB纹理
    D3D11_TEXTURE2D_DESC rgbDesc = {};
    rgbDesc.Width = width;
    rgbDesc.Height = height;
    rgbDesc.MipLevels = 1;
    rgbDesc.ArraySize = 1;
    rgbDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // BGRA格式，适合OpenGL
    rgbDesc.SampleDesc.Count = 1;
    rgbDesc.SampleDesc.Quality = 0;
    rgbDesc.Usage = D3D11_USAGE_DEFAULT;
    rgbDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    rgbDesc.CPUAccessFlags = 0;
    rgbDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr = d3d11Device_->CreateTexture2D(&rgbDesc, nullptr, &outputRGBTexture_);
    if (FAILED(hr)) {
        qDebug() << "Failed to create RGB texture";
        return false;
    }

    // 获取共享句柄
    ComPtr<IDXGIResource> dxgiResource;
    hr = outputRGBTexture_->QueryInterface(__uuidof(IDXGIResource), (void **)&dxgiResource);
    if (FAILED(hr)) {
        qDebug() << "Failed to query DXGI resource";
        return false;
    }

    hr = dxgiResource->GetSharedHandle(&rgbSharedHandle_);
    if (FAILED(hr)) {
        qDebug() << "Failed to get shared handle";
        return false;
    }

    // 创建VideoProcessorOutputView
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {};
    outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDesc.Texture2D.MipSlice = 0;

    hr = videoDevice_->CreateVideoProcessorOutputView(
        outputRGBTexture_.Get(), videoProcessorEnum_.Get(), &outputViewDesc, &outputView_);
    if (FAILED(hr)) {
        qDebug() << "Failed to create VideoProcessorOutputView";
        return false;
    }

    qDebug() << "RGB texture created successfully";
    return true;
}

bool Nv12Render_D3d11va::processNV12ToRGB(const decoder_sdk::Frame &frame)
{
    ID3D11Texture2D *sourceTexture = reinterpret_cast<ID3D11Texture2D *>(frame.data(0));
    if (!sourceTexture || !videoProcessor_ || !videoContext_) {
        qDebug() << "Missing required resources";
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc;
    sourceTexture->GetDesc(&srcDesc);

    // 检查是否需要重新创建纹理
    if (!outputRGBTexture_ || currentWidth_ != srcDesc.Width || currentHeight_ != srcDesc.Height) {
        cleanup();

        currentWidth_ = srcDesc.Width;
        currentHeight_ = srcDesc.Height;

        if (!createRGBTexture(currentWidth_, currentHeight_)) {
            return false;
        }

        if (!registerRGBTextureWithOpenGL()) {
            return false;
        }
    }

    // 获取源纹理的共享句柄并在当前设备上打开
    ComPtr<IDXGIResource> dxgiResource;
    HRESULT hr = sourceTexture->QueryInterface(__uuidof(IDXGIResource), (void **)&dxgiResource);
    if (FAILED(hr)) {
        qDebug() << "Failed to query source DXGI resource";
        return false;
    }

    HANDLE sharedHandle = nullptr;
    hr = dxgiResource->GetSharedHandle(&sharedHandle);
    if (FAILED(hr)) {
        qDebug() << "Failed to get source shared handle";
        return false;
    }

    ComPtr<ID3D11Texture2D> sharedSourceTexture;
    hr = d3d11Device_->OpenSharedResource(sharedHandle, __uuidof(ID3D11Texture2D),
                                          (void **)&sharedSourceTexture);
    if (FAILED(hr)) {
        qDebug() << "Failed to open shared source texture";
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
            qDebug() << "Failed to create VideoProcessorInputView";
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
        qDebug() << "Failed to lock WGL objects";
        return false;
    }

    hr = videoContext_->VideoProcessorBlt(videoProcessor_.Get(), outputView_.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        qDebug() << "VideoProcessorBlt failed, HRESULT:" << hr;
        return false;
    }

    // 解锁WGL对象
    if (!wglDXUnlockObjectsNV(wglD3DDevice_, 1, &wglTextureHandle_)) {
        qDebug() << "Failed to unlock WGL objects";
    }

    return true;
}

bool Nv12Render_D3d11va::registerRGBTextureWithOpenGL()
{
    if (!wglD3DDevice_ || !outputRGBTexture_) {
        qDebug() << "Missing resources for OpenGL registration";
        return false;
    }

    // 创建OpenGL纹理
    glGenTextures(1, &glRGBTexture_);
    glBindTexture(GL_TEXTURE_2D, glRGBTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 注册D3D纹理到WGL
    wglTextureHandle_ = wglDXRegisterObjectNV(wglD3DDevice_, outputRGBTexture_.Get(), glRGBTexture_,
                                              GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
    if (!wglTextureHandle_) {
        DWORD error = GetLastError();
        qDebug() << "Failed to register RGB texture with WGL, error:" << error;
        return false;
    }

    qDebug() << "RGB texture registered with OpenGL successfully";
    return true;
}

bool Nv12Render_D3d11va::validateOpenGLContext()
{
    return wglGetCurrentContext() != nullptr;
}

void Nv12Render_D3d11va::clearGL()
{
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

#endif