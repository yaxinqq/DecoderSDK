#ifdef _WIN32
#include <Windows.h>
#endif

#include "Nv12Render_D3d11va.h"
#include <QDebug>

namespace {
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

// 修正的NV12着色器 - 直接从NV12纹理采样
const char *fsrc = R"(
        #extension GL_ARB_texture_rectangle : enable
        precision mediump float;
        uniform sampler2D textureNV12;
        uniform float textureWidth;
        uniform float textureHeight;
        varying vec2 textureOut;
        
        void main(void)
        {
            vec2 texCoord = textureOut;
            
            // 从NV12纹理采样Y值(前2/3部分)
            float y = texture2D(textureNV12, vec2(texCoord.x, texCoord.y * 2.0/3.0)).r;
            
            // 从NV12纹理采样UV值(后1/3部分)
            vec2 uvCoord = vec2(texCoord.x, 2.0/3.0 + texCoord.y * 1.0/3.0 * 0.5);
            vec2 uv = texture2D(textureNV12, uvCoord).rg;
            
            // 标准BT.709 YUV到RGB转换
            const vec3 yuv2rgb_ofs = vec3(-0.0625, -0.5, -0.5);
            const mat3 yuv2rgb_mat = mat3(
                1.164,  0.0,    1.596,
                1.164, -0.391, -0.813,
                1.164,  2.018,  0.0
            );
            
            // YUV到RGB的转换
            vec3 rgb = (vec3(y, uv.r, uv.g) + yuv2rgb_ofs) * yuv2rgb_mat;
            gl_FragColor = vec4(rgb, 1.0);
        }
    )";
} // namespace

Nv12Render_D3d11va::Nv12Render_D3d11va(ID3D11Device *d3d11Device) : d3d11Device_(d3d11Device)
{
    if (!d3d11Device_) {
        initializeD3D11();
        ownD3DDevice_ = true;
    }

    if (d3d11Device_) {
        d3d11Device_->GetImmediateContext(&d3d11Context_);
    }

    initializeWGL();
}

Nv12Render_D3d11va::~Nv12Render_D3d11va()
{
    cleanupWGL();
    if (ownD3DDevice_) {
        cleanupD3D11();
    }

    if (d3d11Context_) {
        d3d11Context_->Release();
        d3d11Context_ = nullptr;
    }

    vbo_.destroy();

    // 清理OpenGL纹理
    if (nv12Texture_)
        glDeleteTextures(1, &nv12Texture_);
}

bool Nv12Render_D3d11va::initializeD3D11()
{
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &d3d11Device_, &featureLevel, &d3d11Context_);

    if (FAILED(hr)) {
        qDebug() << "Failed to create D3D11 device, HRESULT:" << hr;
        return false;
    }

    qDebug() << "D3D11 device created successfully, feature level:" << featureLevel;
    return true;
}

bool Nv12Render_D3d11va::initializeWGL()
{
    // 获取WGL扩展函数指针
    wglDXOpenDeviceNV = (PFNWGLDXOPENDEVICENVPROC)wglGetProcAddress("wglDXOpenDeviceNV");
    wglDXCloseDeviceNV = (PFNWGLDXCLOSEDEVICENVPROC)wglGetProcAddress("wglDXCloseDeviceNV");
    wglDXRegisterObjectNV =
        (PFNWGLDXREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXRegisterObjectNV");
    wglDXUnregisterObjectNV =
        (PFNWGLDXUNREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXUnregisterObjectNV");
    wglDXLockObjectsNV = (PFNWGLDXLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXLockObjectsNV");
    wglDXUnlockObjectsNV = (PFNWGLDXUNLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXUnlockObjectsNV");

    if (!wglDXOpenDeviceNV || !wglDXCloseDeviceNV || !wglDXRegisterObjectNV ||
        !wglDXUnregisterObjectNV || !wglDXLockObjectsNV || !wglDXUnlockObjectsNV) {
        qDebug() << "WGL_NV_DX_interop extension not available";
        return false;
    }

    // 打开D3D设备用于互操作
    wglD3DDevice_ = wglDXOpenDeviceNV(d3d11Device_);
    if (!wglD3DDevice_) {
        qDebug() << "Failed to open D3D11 device for WGL interop";
        return false;
    }

    qDebug() << "WGL D3D11 interop initialized successfully";
    return true;
}

void Nv12Render_D3d11va::initialize(const int width, const int height, const bool horizontal,
                                    const bool vertical)
{
    initializeOpenGLFunctions();

    videoWidth_ = width;
    videoHeight_ = height;

    qDebug() << "Initializing Nv12Render_D3d11va with size:" << width << "x" << height;

    // 编译着色器
    if (!program_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vsrc)) {
        qDebug() << "Failed to compile vertex shader:" << program_.log();
        return;
    }
    if (!program_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fsrc)) {
        qDebug() << "Failed to compile fragment shader:" << program_.log();
        return;
    }
    if (!program_.link()) {
        qDebug() << "Failed to link shader program:" << program_.log();
        return;
    }

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
        // 纹理坐标（根据horizontal和vertical调整）
        horizontal ? 1.0f : 0.0f,
        vertical ? 1.0f : 0.0f,
        horizontal ? 0.0f : 1.0f,
        vertical ? 1.0f : 0.0f,
        horizontal ? 1.0f : 0.0f,
        vertical ? 0.0f : 1.0f,
        horizontal ? 0.0f : 1.0f,
        vertical ? 0.0f : 1.0f,
    };

    vbo_.create();
    vbo_.bind();
    vbo_.allocate(points, sizeof(points));

    // 创建OpenGL纹理
    glGenTextures(1, &nv12Texture_);
    glBindTexture(GL_TEXTURE_2D, nv12Texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    qDebug() << "Nv12Render_D3d11va initialization completed";
    clearGL();
}

void Nv12Render_D3d11va::render(const decoder_sdk::Frame &frame)
{
    if (!frame.isValid() || frame.pixelFormat() != decoder_sdk::ImageFormat::kD3d11va) {
        qDebug() << "Frame validation failed - Valid:" << frame.isValid()
                 << "Format:" << static_cast<int>(frame.pixelFormat())
                 << "Expected:" << static_cast<int>(decoder_sdk::ImageFormat::kD3d11va);
        clearGL();
        return;
    }

    // 获取D3D11纹理数据
    ID3D11Texture2D *texture = reinterpret_cast<ID3D11Texture2D *>(frame.data(0));
    if (!texture) {
        qDebug() << "D3D11 texture pointer is null";
        return;
    }

    // 获取纹理数组索引
    UINT textureIndex = static_cast<UINT>(reinterpret_cast<intptr_t>(frame.data(1)));

    // 获取源纹理描述
    D3D11_TEXTURE2D_DESC srcDesc;
    texture->GetDesc(&srcDesc);

    qDebug() << "Source texture info:"
             << "Format:" << srcDesc.Format << "Size:" << srcDesc.Width << "x" << srcDesc.Height
             << "ArraySize:" << srcDesc.ArraySize << "Index:" << textureIndex;

    // 验证纹理格式
    if (srcDesc.Format != DXGI_FORMAT_NV12) {
        qDebug() << "Warning: Expected DXGI_FORMAT_NV12 (103), got format:" << srcDesc.Format;
    }

    // 验证纹理数组索引
    if (textureIndex >= srcDesc.ArraySize) {
        qDebug() << "Error: Texture index" << textureIndex << "exceeds array size"
                 << srcDesc.ArraySize;
        return;
    }

    QMutexLocker lock(&mtx_);

    // 创建单独的纹理来复制数据，而不是直接注册纹理数组
    if (!copyTextureData(texture, textureIndex)) {
        qDebug() << "Failed to copy texture data";
        return;
    }
}

// 新增方法：复制纹理数据到单独的纹理
bool Nv12Render_D3d11va::copyTextureData(ID3D11Texture2D *sourceTexture, UINT textureIndex)
{
    if (!sourceTexture || !d3d11Context_) {
        return false;
    }

    // 获取源纹理描述
    D3D11_TEXTURE2D_DESC srcDesc;
    sourceTexture->GetDesc(&srcDesc);

    // 如果目标纹理不存在或尺寸不匹配，重新创建
    if (!targetTexture_ || targetTextureWidth_ != srcDesc.Width ||
        targetTextureHeight_ != srcDesc.Height) {
        // 清理旧纹理
        if (targetTexture_) {
            if (wglD3DTextureHandle_) {
                wglDXUnregisterObjectNV(wglD3DDevice_, wglD3DTextureHandle_);
                wglD3DTextureHandle_ = nullptr;
            }
            targetTexture_->Release();
            targetTexture_ = nullptr;
        }

        // 创建新的目标纹理（非数组纹理）
        D3D11_TEXTURE2D_DESC targetDesc = srcDesc;
        targetDesc.ArraySize = 1;                          // 单个纹理，不是数组
        targetDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // 用于WGL互操作
        targetDesc.Usage = D3D11_USAGE_DEFAULT;
        targetDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = d3d11Device_->CreateTexture2D(&targetDesc, nullptr, &targetTexture_);
        if (FAILED(hr)) {
            qDebug() << "Failed to create target texture, HRESULT:" << hr;
            return false;
        }

        targetTextureWidth_ = srcDesc.Width;
        targetTextureHeight_ = srcDesc.Height;

        // 注册新纹理到WGL
        wglD3DTextureHandle_ = wglDXRegisterObjectNV(wglD3DDevice_, targetTexture_, nv12Texture_,
                                                     GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
        if (!wglD3DTextureHandle_) {
            qDebug() << "Failed to register target texture with WGL";
            return false;
        }
    }

    // 复制纹理数组中的特定索引到目标纹理
    d3d11Context_->CopySubresourceRegion(targetTexture_, // 目标纹理
                                         0,              // 目标子资源索引
                                         0, 0, 0,        // 目标位置
                                         sourceTexture,  // 源纹理
                                         textureIndex,   // 源子资源索引（纹理数组索引）
                                         nullptr         // 复制整个纹理
    );

    return true;
}

void Nv12Render_D3d11va::draw()
{
    QMutexLocker lock(&mtx_);

    if (!wglD3DTextureHandle_ || resourceRegisteredFailed_) {
        clearGL();
        return;
    }

    // 锁定纹理进行OpenGL访问
    if (!wglDXLockObjectsNV(wglD3DDevice_, 1, &wglD3DTextureHandle_)) {
        qDebug() << "Failed to lock NV12 texture";
        clearGL();
        return;
    }

    program_.bind();
    vbo_.bind();

    program_.enableAttributeArray("vertexIn");
    program_.enableAttributeArray("textureIn");
    program_.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 2, 0);
    program_.setAttributeBuffer("textureIn", GL_FLOAT, 2 * 4 * sizeof(GLfloat), 2, 0);

    // 绑定NV12纹理
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, nv12Texture_);

    program_.setUniformValue("textureNV12", 0);
    program_.setUniformValue("textureWidth", static_cast<float>(videoWidth_));
    program_.setUniformValue("textureHeight", static_cast<float>(videoHeight_));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    program_.disableAttributeArray("vertexIn");
    program_.disableAttributeArray("textureIn");
    vbo_.release();
    program_.release();

    // 解锁纹理
    if (!wglDXUnlockObjectsNV(wglD3DDevice_, 1, &wglD3DTextureHandle_)) {
        qDebug() << "Failed to unlock NV12 texture";
    }
}

QOpenGLFramebufferObject *Nv12Render_D3d11va::getFrameBuffer(const QSize &size)
{
    QMutexLocker lock(&mtx_);

    QOpenGLFramebufferObject *frameBuffer = new QOpenGLFramebufferObject(size);
    frameBuffer->bind();

    draw();

    return frameBuffer;
}

void Nv12Render_D3d11va::clearGL()
{
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void Nv12Render_D3d11va::cleanupD3D11()
{
    if (d3d11Context_) {
        d3d11Context_->Release();
        d3d11Context_ = nullptr;
    }
    if (d3d11Device_) {
        d3d11Device_->Release();
        d3d11Device_ = nullptr;
    }
}

void Nv12Render_D3d11va::cleanupWGL()
{
    // 注销WGL对象
    if (wglD3DTextureHandle_) {
        wglDXUnregisterObjectNV(wglD3DDevice_, wglD3DTextureHandle_);
        wglD3DTextureHandle_ = nullptr;
    }

    if (wglD3DDevice_) {
        wglDXCloseDeviceNV(wglD3DDevice_);
        wglD3DDevice_ = nullptr;
    }
}