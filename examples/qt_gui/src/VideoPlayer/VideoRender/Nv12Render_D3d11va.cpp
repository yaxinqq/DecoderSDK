#include "Nv12Render_D3d11va.h"

#ifdef _WIN32
#include <Windows.h>
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
} // namespace

Nv12Render_D3d11va::Nv12Render_D3d11va(ID3D11Device *d3d11Device)
    : d3d11Device_(d3d11Device), d3d11Device3_(nullptr)
{
    qDebug() << "Constructor called";

    if (!d3d11Device_) {
        qDebug() << "No D3D11 device provided, creating own";
        initializeD3D11();
        ownD3DDevice_ = true;
    }

    if (d3d11Device_) {
        d3d11Device_->GetImmediateContext(&d3d11Context_);

        // 查询D3D11.1接口
        HRESULT hr = d3d11Device_->QueryInterface(__uuidof(ID3D11Device1), (void **)&d3d11Device3_);
        if (FAILED(hr)) {
            qDebug() << "Failed to get D3D11.1 interface, HRESULT:" << hr;
        } else {
            qDebug() << "D3D11.1 interface obtained successfully";
        }

        qDebug() << "D3D11 context obtained";
    }
}

Nv12Render_D3d11va::~Nv12Render_D3d11va()
{
    qDebug() << "Destructor called";

    // 清理所有资源
    cleanup();

    // 释放D3D11.1接口
    if (d3d11Device3_) {
        d3d11Device3_->Release();
        d3d11Device3_ = nullptr;
    }

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
        d3d11Context_->Release();
        d3d11Context_ = nullptr;
    }

    // 如果拥有D3D设备，则释放它
    if (ownD3DDevice_ && d3d11Device_) {
        d3d11Device_->Release();
        d3d11Device_ = nullptr;
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

    // 从Frame中提取D3D11纹理
    ID3D11Texture2D *d3d11Texture = reinterpret_cast<ID3D11Texture2D *>(frame.data(0));
    if (!d3d11Texture) {
        qDebug() << "D3D11 texture pointer is null";
        return;
    }

    // 创建NV12共享纹理
    if (!createNV12SharedTextures(d3d11Texture)) {
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

    program_.setUniformValue("textureY", 1);
    program_.setUniformValue("textureUV", 0);

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
    wglD3DDevice_ = wglDXOpenDeviceNV(d3d11Device_);
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

    // 清理D3D11 SRV
    if (ySRV_) {
        qDebug() << "Releasing Y SRV";
        ySRV_->Release();
        ySRV_ = nullptr;
    }

    if (uvSRV_) {
        qDebug() << "Releasing UV SRV";
        uvSRV_->Release();
        uvSRV_ = nullptr;
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

bool Nv12Render_D3d11va::createNV12SharedTextures(ID3D11Texture2D *sourceTexture)
{
    if (!sourceTexture || !d3d11Context_ || !wglD3DDevice_) {
        qDebug() << "Missing required resources";
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc;
    sourceTexture->GetDesc(&srcDesc);

    qDebug() << "Source texture size:" << srcDesc.Width << "x" << srcDesc.Height
             << "Format:" << srcDesc.Format;

    // 检查是否需要重新创建纹理
    if (!nv12Texture_ || currentWidth_ != srcDesc.Width || currentHeight_ != srcDesc.Height) {
        qDebug() << "Creating new NV12 shared texture";

        // 清理旧资源
        cleanup();

        // 创建新的NV12共享纹理
        D3D11_TEXTURE2D_DESC nv12Desc = {};
        nv12Desc.Width = srcDesc.Width;
        nv12Desc.Height = srcDesc.Height;
        nv12Desc.MipLevels = 1;
        nv12Desc.ArraySize = 1;
        nv12Desc.Format = DXGI_FORMAT_NV12;
        nv12Desc.SampleDesc.Count = 1;
        nv12Desc.Usage = D3D11_USAGE_DEFAULT;
        nv12Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        nv12Desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        HRESULT hr = d3d11Device_->CreateTexture2D(&nv12Desc, nullptr, &nv12Texture_);
        if (FAILED(hr)) {
            qDebug() << "Failed to create NV12 texture, HRESULT:" << hr;
            return false;
        }

        currentWidth_ = srcDesc.Width;
        currentHeight_ = srcDesc.Height;

        // 创建Y和UV平面的SRV
        if (!createPlaneShaderResourceViews(nv12Texture_)) {
            qDebug() << "Failed to create plane SRVs";
            return false;
        }

        // 注册平面到OpenGL
        if (!registerPlanesWithOpenGL()) {
            qDebug() << "Failed to register planes with OpenGL";
            return false;
        }
    }

    // 锁定WGL对象
    HANDLE handles[] = {wglTextureHandleY_, wglTextureHandleUV_};
    if (!wglDXLockObjectsNV(wglD3DDevice_, 2, handles)) {
        DWORD error = GetLastError();
        qDebug() << "Failed to lock WGL objects, error:" << error;
        return false;
    }

    // 复制源纹理到NV12纹理
    d3d11Context_->CopyResource(nv12Texture_, sourceTexture);
    d3d11Context_->Flush();

    // 解锁WGL对象
    if (!wglDXUnlockObjectsNV(wglD3DDevice_, 2, handles)) {
        DWORD error = GetLastError();
        qDebug() << "Failed to unlock WGL objects, error:" << error;
        return false;
    }

    qDebug() << "NV12 shared textures created and updated successfully";
    return true;
}

bool Nv12Render_D3d11va::createPlaneShaderResourceViews(ID3D11Texture2D *nv12Texture)
{
    if (!nv12Texture || !d3d11Device3_) {
        qDebug() << "Invalid texture or D3D11.1 device not available";
        return false;
    }

    // 清理之前的资源
    if (ySRV_) {
        ySRV_->Release();
        ySRV_ = nullptr;
    }
    if (uvSRV_) {
        uvSRV_->Release();
        uvSRV_ = nullptr;
    }

    // 创建Y平面SRV (使用PlaneSlice = 0)
    D3D11_SHADER_RESOURCE_VIEW_DESC1 ySrvDesc = {};
    ySrvDesc.Format = DXGI_FORMAT_R8_UNORM;
    ySrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    ySrvDesc.Texture2D.MostDetailedMip = 0;
    ySrvDesc.Texture2D.MipLevels = 1;
    ySrvDesc.Texture2D.PlaneSlice = 0; // Y平面

    HRESULT hr = d3d11Device3_->CreateShaderResourceView1(nv12Texture, &ySrvDesc, &ySRV_);
    if (FAILED(hr)) {
        qDebug() << "Failed to create Y plane SRV, HRESULT:" << hr;
        return false;
    }

    // 创建UV平面SRV (使用PlaneSlice = 1)
    D3D11_SHADER_RESOURCE_VIEW_DESC1 uvSrvDesc = {};
    uvSrvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    uvSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    uvSrvDesc.Texture2D.MostDetailedMip = 0;
    uvSrvDesc.Texture2D.MipLevels = 1;
    uvSrvDesc.Texture2D.PlaneSlice = 1; // UV平面

    hr = d3d11Device3_->CreateShaderResourceView1(nv12Texture, &uvSrvDesc, &uvSRV_);
    if (FAILED(hr)) {
        qDebug() << "Failed to create UV plane SRV, HRESULT:" << hr;
        ySRV_->Release();
        ySRV_ = nullptr;
        return false;
    }

    // 保存原始NV12纹理的引用用于WGL注册
    if (yTexture_) {
        yTexture_->Release();
    }
    if (uvTexture_) {
        uvTexture_->Release();
    }

    // 直接使用原始NV12纹理
    yTexture_ = nv12Texture;
    uvTexture_ = nv12Texture;
    yTexture_->AddRef();
    uvTexture_->AddRef();

    qDebug() << "Successfully created Y and UV plane SRVs using D3D11.1";
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, currentWidth_, currentHeight_, 0, GL_RG, GL_UNSIGNED_BYTE,
                 nullptr);
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
    wglTextureHandleY_ = wglDXRegisterObjectNV(wglD3DDevice_, yTexture_, glTextureY_, GL_TEXTURE_2D,
                                               WGL_ACCESS_READ_ONLY_NV);
    if (!wglTextureHandleY_) {
        DWORD error = GetLastError();
        qDebug() << "Failed to register Y plane with WGL, error:" << error;
        return false;
    }

    // 注册UV平面到WGL - 使用原始纹理
    wglTextureHandleUV_ = wglDXRegisterObjectNV(wglD3DDevice_, uvTexture_, glTextureUV_,
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