#ifdef D3D11VA_AVAILABLE
#include "Nv12Render_D3d11va.h"
#include "Commonutils.h"

#ifdef _WIN32
#include <Windows.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#endif

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
    : VideoRender(),
      d3d11Device_(d3d11_utils::getD3D11Device()),
      wglD3DDevice_(d3d11_utils::getWglDeviceRef())
{
    if (d3d11Device_) {
        d3d11Device_->GetImmediateContext(&d3d11Context_);
    }
}

Nv12Render_D3d11va::~Nv12Render_D3d11va()
{
    cleanup();

    vbo_.destroy();

    if (d3d11Context_) {
        d3d11Context_->ClearState();
        d3d11Context_->Flush();
        d3d11Context_.Reset();
    }
    if (d3d11Device_) {
        d3d11Device_.Reset();
    }
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
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Failed to initialize WGL interop!");
        return false;
    }

    if (!initializeVideoProcessor(frame.width(), frame.height())) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Failed to initialize VideoProcessor!");
        return false;
    }

    if (!registerTextureWithOpenGL(frame.width(), frame.height())) {
        qWarning() << QStringLiteral(
            "[Nv12Render_D3d11va] Failed to register D3D texture to OpenGL texture!");
        return false;
    }

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

    return drawFrame(glRGBTexture_);
}

bool Nv12Render_D3d11va::initializeWGLInterop()
{
    if (!d3d11Device_) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] D3D11 device is null!");
        return false;
    }

    HRESULT hr = d3d11Device_->GetDeviceRemovedReason();
    if (FAILED(hr)) {
        qWarning() << QStringLiteral(
                          "[Nv12Render_D3d11va] D3D11 device is in error state, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    return true;
}

bool Nv12Render_D3d11va::initializeVideoProcessor(int width, int height)
{
    if (!d3d11Context_) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] D3D11 context is invalid!");
        return false;
    }

    // 获取VideoDevice接口
    HRESULT hr = d3d11Device_->QueryInterface(__uuidof(ID3D11VideoDevice), (void **)&videoDevice_);
    if (FAILED(hr)) {
        qWarning() << QStringLiteral(
                          "[Nv12Render_D3d11va] Failed to get VideoDevice interface, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    // 获取VideoContext接口
    hr = d3d11Context_->QueryInterface(__uuidof(ID3D11VideoContext), (void **)&videoContext_);
    if (FAILED(hr)) {
        qWarning() << QStringLiteral(
                          "[Nv12Render_D3d11va] Failed to get VideoContext interface, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    // 创建VideoProcessorEnumerator
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = width;
    contentDesc.InputHeight = height;
    contentDesc.OutputWidth = width;
    contentDesc.OutputHeight = height;
    contentDesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

    hr = videoDevice_->CreateVideoProcessorEnumerator(&contentDesc, &videoProcessorEnum_);
    if (FAILED(hr)) {
        qWarning() << QStringLiteral("Failed to create VideoProcessorEnumerator, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    // 创建VideoProcessor
    hr = videoDevice_->CreateVideoProcessor(videoProcessorEnum_.Get(), 0, &videoProcessor_);
    if (FAILED(hr)) {
        qWarning() << QStringLiteral(
                          "[Nv12Render_D3d11va] Failed to create VideoProcessor, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    return true;
}

void Nv12Render_D3d11va::cleanup()
{
    // 确保WGL对象正确注销
    if (wglTextureHandle_ && wglD3DDevice_.isValid()) {
        if (!wglD3DDevice_.wglDXUnregisterObjectNV(wglTextureHandle_)) {
            qWarning() << QStringLiteral("[Nv12Render_D3d11va] Failed to unregister WGL object!");
        }
        wglTextureHandle_ = nullptr;
    }

    if (glRGBTexture_) {
        glDeleteTextures(1, &glRGBTexture_);
        glRGBTexture_ = 0;
    }

    // 清理资源缓存
    resourceCache_.clear();

    // 清理VideoProcessor资源
    if (inputView_) {
        inputView_.Reset();
    }
    if (outputView_) {
        outputView_.Reset();
    }
    if (videoProcessor_)
        videoProcessor_.Reset();
    if (videoProcessorEnum_)
        videoProcessorEnum_.Reset();
    if (videoContext_)
        videoContext_.Reset();
    if (videoDevice_)
        videoDevice_.Reset();

    // 清理纹理 - 确保正确释放
    if (inputNV12Texture_) {
        inputNV12Texture_.Reset();
    }
    if (outputRGBTexture_) {
        outputRGBTexture_.Reset();
    }
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
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Missing required resources!");
        return false;
    }

    // 使用资源缓存避免频繁重建
    auto cacheKey = std::make_pair(reinterpret_cast<uintptr_t>(sourceTexture),
                                   static_cast<UINT>(reinterpret_cast<intptr_t>(frame.data(1))));

    ResourceCacheEntry entry;
    if (1) {
        // 检查设备是否相同
        ComPtr<ID3D11Device> sourceDevice;
        sourceTexture->GetDevice(&sourceDevice);

        if (sourceDevice.Get() == d3d11Device_.Get()) {
            entry.inputTexture = sourceTexture;
        } else {
            // 不同设备，使用共享句柄
            ComPtr<IDXGIResource> dxgiResource;
            HRESULT hr =
                sourceTexture->QueryInterface(__uuidof(IDXGIResource), (void **)&dxgiResource);
            if (FAILED(hr)) {
                qWarning()
                    << QStringLiteral(
                           "[Nv12Render_D3d11va] Failed to query source DXGI resource, HRESULT:")
                    << Qt::hex << hr;
                return false;
            }

            HANDLE sharedHandle = nullptr;
            hr = dxgiResource->GetSharedHandle(&sharedHandle);
            if (FAILED(hr)) {
                qWarning()
                    << QStringLiteral(
                           "[Nv12Render_D3d11va] Failed to get source shared handle, HRESULT:")
                    << Qt::hex << hr;
                return false;
            }

            hr = d3d11Device_->OpenSharedResource(sharedHandle, __uuidof(ID3D11Texture2D),
                                                  (void **)&entry.inputTexture);
            if (FAILED(hr)) {
                qWarning()
                    << QStringLiteral(
                           "[Nv12Render_D3d11va] Failed to open shared source texture, HRESULT:")
                    << Qt::hex << hr;
                return false;
            }
        }

        // 创建InputView
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {};
        inputViewDesc.FourCC = 0;
        inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputViewDesc.Texture2D.MipSlice = 0;
        inputViewDesc.Texture2D.ArraySlice =
            static_cast<UINT>(reinterpret_cast<intptr_t>(frame.data(1)));

        HRESULT hr = videoDevice_->CreateVideoProcessorInputView(
            entry.inputTexture.Get(), videoProcessorEnum_.Get(), &inputViewDesc, &entry.inputView);
        if (FAILED(hr)) {
            qWarning()
                << QStringLiteral(
                       "[Nv12Render_D3d11va] Failed to create VideoProcessorInputView, HRESULT:")
                << Qt::hex << hr;
            return false;
        }
    }

    // 设置颜色空间
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColorSpace = {};
    inputColorSpace.YCbCr_Matrix = 1; // BT.709
    inputColorSpace.YCbCr_xvYCC = 0;
    inputColorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    inputColorSpace.Usage = 0; // 视频内容

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace = {};
    outputColorSpace.YCbCr_Matrix = 0; // RGB矩阵
    outputColorSpace.RGB_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    outputColorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    outputColorSpace.Usage = 0; // 视频内容

    videoContext_->VideoProcessorSetStreamColorSpace(videoProcessor_.Get(), 0, &inputColorSpace);
    videoContext_->VideoProcessorSetOutputColorSpace(videoProcessor_.Get(), &outputColorSpace);

    // 添加源矩形和目标矩形设置
    // 获取源纹理描述
    D3D11_TEXTURE2D_DESC sourceDesc;
    entry.inputTexture->GetDesc(&sourceDesc);

    // 获取输出纹理描述
    D3D11_TEXTURE2D_DESC outputDesc;
    outputRGBTexture_->GetDesc(&outputDesc);

    // 设置源矩形（实际视频内容区域）
    RECT sourceRect = {
        0, 0,
        static_cast<LONG>(frame.width()), // 使用实际视频宽度
        static_cast<LONG>(frame.height()) // 使用实际视频高度
    };
    videoContext_->VideoProcessorSetStreamSourceRect(videoProcessor_.Get(), 0, TRUE, &sourceRect);

    // 设置目标矩形（输出纹理区域）
    RECT destRect = {0, 0, static_cast<LONG>(outputDesc.Width),
                     static_cast<LONG>(outputDesc.Height)};
    videoContext_->VideoProcessorSetStreamDestRect(videoProcessor_.Get(), 0, TRUE, &destRect);

    // 执行颜色空间转换
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = entry.inputView.Get();

    wglD3DDevice_.wglDXUnlockObjectsNV(1, &wglTextureHandle_);
    HRESULT hr =
        videoContext_->VideoProcessorBlt(videoProcessor_.Get(), outputView_.Get(), 0, 1, &stream);
    wglD3DDevice_.wglDXLockObjectsNV(1, &wglTextureHandle_);

    if (FAILED(hr)) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] VideoProcessorBlt failed, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    entry.inputTexture.Reset();
    entry.inputView.Reset();

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

    // 获取共享句柄
    ComPtr<IDXGIResource> dxgiResource;
    hr = outputRGBTexture_->QueryInterface(__uuidof(IDXGIResource), (void **)&dxgiResource);
    if (FAILED(hr)) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Failed to query DXGI resource, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    hr = dxgiResource->GetSharedHandle(&rgbSharedHandle_);
    if (FAILED(hr)) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Failed to get shared handle, HRESULT:")
                   << Qt::hex << hr;
        return false;
    }

    // 创建VideoProcessorOutputView
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {};
    outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDesc.Texture2D.MipSlice = 0;

    hr = videoDevice_->CreateVideoProcessorOutputView(
        outputRGBTexture_.Get(), videoProcessorEnum_.Get(), &outputViewDesc, &outputView_);
    if (FAILED(hr)) {
        qWarning()
            << QStringLiteral(
                   "[Nv12Render_D3d11va] Failed to create VideoProcessorOutputView, HRESULT:")
            << Qt::hex << hr;
        return false;
    }

    if (!wglD3DDevice_.isValid() || !outputRGBTexture_) {
        qWarning() << QStringLiteral(
            "[Nv12Render_D3d11va] Missing resources for OpenGL registration!");
        return false;
    }

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

    return true;
}

bool Nv12Render_D3d11va::drawFrame(GLuint id)
{
    // 资源未就绪
    if (!program_.isLinked() || !wglTextureHandle_ || !outputRGBTexture_) {
        qWarning() << QStringLiteral("[Nv12Render_D3d11va] Not ready for drawing!");
        return false;
    }

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

    return true;
}

#endif