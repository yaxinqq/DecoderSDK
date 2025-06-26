#ifdef _WIN32
#include <Windows.h>
#endif

#include "Nv12Render_Dxva2.h"
#include <QDebug>

namespace
{
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

    const char *fsrc = R"(
        precision mediump float;
        uniform sampler2D textureY;
        uniform sampler2D textureUV;
        varying vec2 textureOut;
        
        void main(void)
        {
            float y = texture2D(textureY, textureOut).r;
            vec2 uv = texture2D(textureUV, textureOut).rg;
            
            const vec3 yuv2rgb_ofs = vec3(0.0625, 0.5, 0.5);
            const mat3 yuv2rgb_mat = mat3(
                1.16438356,  0.0,           1.79274107,
                1.16438356, -0.21324861, -0.53290932,
                1.16438356,  2.11240178,  0.0
            );
            
            vec3 rgb = (vec3(y, uv.r, uv.g) - yuv2rgb_ofs) * yuv2rgb_mat;
            gl_FragColor = vec4(rgb, 1.0);
        }
    )";
}

Nv12Render_Dxva2::Nv12Render_Dxva2(IDirect3DDevice9 *d3dDevice)
    : d3dDevice_(d3dDevice)
{
    if (!d3dDevice_)
    {
        initializeD3D9();
        ownD3DDevice_ = true;
    }
    initializeWGL();
}

Nv12Render_Dxva2::~Nv12Render_Dxva2()
{
    cleanupWGL();
    if (ownD3DDevice_)
    {
        cleanupD3D9();
    }

    vbo_.destroy();

    // 清理双缓冲纹理
    if (textureCurrentY_)
        glDeleteTextures(1, &textureCurrentY_);
    if (textureCurrentUV_)
        glDeleteTextures(1, &textureCurrentUV_);
    if (textureNextY_)
        glDeleteTextures(1, &textureNextY_);
    if (textureNextUV_)
        glDeleteTextures(1, &textureNextUV_);

    // 清理D3D表面
    if (d3dSurfaceCurrentY_)
    {
        d3dSurfaceCurrentY_->Release();
        d3dSurfaceCurrentY_ = nullptr;
    }
    if (d3dSurfaceCurrentUV_)
    {
        d3dSurfaceCurrentUV_->Release();
        d3dSurfaceCurrentUV_ = nullptr;
    }
    if (d3dSurfaceNextY_)
    {
        d3dSurfaceNextY_->Release();
        d3dSurfaceNextY_ = nullptr;
    }
    if (d3dSurfaceNextUV_)
    {
        d3dSurfaceNextUV_->Release();
        d3dSurfaceNextUV_ = nullptr;
    }
}

bool Nv12Render_Dxva2::initializeD3D9()
{
    d3d9_ = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d9_)
    {
        qDebug() << "Failed to create D3D9 object";
        return false;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = GetDesktopWindow();

    HRESULT hr = d3d9_->CreateDevice(
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        GetDesktopWindow(),
        D3DCREATE_HARDWARE_VERTEXPROCESSING,
        &pp,
        &d3dDevice_);

    if (FAILED(hr))
    {
        qDebug() << "Failed to create D3D9 device";
        return false;
    }

    return true;
}

bool Nv12Render_Dxva2::initializeWGL()
{
    // 获取WGL扩展函数指针
    wglDXOpenDeviceNV = (PFNWGLDXOPENDEVICENVPROC)wglGetProcAddress("wglDXOpenDeviceNV");
    wglDXCloseDeviceNV = (PFNWGLDXCLOSEDEVICENVPROC)wglGetProcAddress("wglDXCloseDeviceNV");
    wglDXRegisterObjectNV = (PFNWGLDXREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXRegisterObjectNV");
    wglDXUnregisterObjectNV = (PFNWGLDXUNREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXUnregisterObjectNV");
    wglDXLockObjectsNV = (PFNWGLDXLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXLockObjectsNV");
    wglDXUnlockObjectsNV = (PFNWGLDXUNLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXUnlockObjectsNV");

    if (!wglDXOpenDeviceNV || !wglDXCloseDeviceNV || !wglDXRegisterObjectNV ||
        !wglDXUnregisterObjectNV || !wglDXLockObjectsNV || !wglDXUnlockObjectsNV)
    {
        qDebug() << "WGL_NV_DX_interop extension not available";
        return false;
    }

    // 打开D3D设备用于互操作
    wglD3DDevice_ = wglDXOpenDeviceNV(d3dDevice_);
    if (!wglD3DDevice_)
    {
        qDebug() << "Failed to open D3D device for WGL interop";
        return false;
    }

    return true;
}

void Nv12Render_Dxva2::initialize(const int width, const int height, const bool horizontal, const bool vertical)
{
    initializeOpenGLFunctions();

    videoWidth_ = width;
    videoHeight_ = height;

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

    // 创建双缓冲OpenGL纹理
    glGenTextures(1, &textureCurrentY_);
    glGenTextures(1, &textureCurrentUV_);
    glGenTextures(1, &textureNextY_);
    glGenTextures(1, &textureNextUV_);

    // 创建双缓冲D3D表面 - Y平面
    HRESULT hr = d3dDevice_->CreateOffscreenPlainSurface(
        width, height,
        D3DFMT_L8, // Y平面使用L8格式
        D3DPOOL_DEFAULT,
        &d3dSurfaceCurrentY_,
        nullptr);

    hr = d3dDevice_->CreateOffscreenPlainSurface(
        width, height,
        D3DFMT_L8,
        D3DPOOL_DEFAULT,
        &d3dSurfaceNextY_,
        nullptr);

    // 创建双缓冲D3D表面 - UV平面
    hr = d3dDevice_->CreateOffscreenPlainSurface(
        width / 2, height / 2,
        D3DFMT_A8L8, // UV平面使用A8L8格式
        D3DPOOL_DEFAULT,
        &d3dSurfaceCurrentUV_,
        nullptr);

    hr = d3dDevice_->CreateOffscreenPlainSurface(
        width / 2, height / 2,
        D3DFMT_A8L8,
        D3DPOOL_DEFAULT,
        &d3dSurfaceNextUV_,
        nullptr);

    // 注册D3D表面到OpenGL
    if (d3dSurfaceCurrentY_)
    {
        wglD3DSurfaceCurrentY_ = wglDXRegisterObjectNV(
            wglD3DDevice_, d3dSurfaceCurrentY_, textureCurrentY_,
            GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
        resourceCurrentYRegisteredFailed_ = (wglD3DSurfaceCurrentY_ == nullptr);
    }

    if (d3dSurfaceCurrentUV_)
    {
        wglD3DSurfaceCurrentUV_ = wglDXRegisterObjectNV(
            wglD3DDevice_, d3dSurfaceCurrentUV_, textureCurrentUV_,
            GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
        resourceCurrentUVRegisteredFailed_ = (wglD3DSurfaceCurrentUV_ == nullptr);
    }

    if (d3dSurfaceNextY_)
    {
        wglD3DSurfaceNextY_ = wglDXRegisterObjectNV(
            wglD3DDevice_, d3dSurfaceNextY_, textureNextY_,
            GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
        resourceNextYRegisteredFailed_ = (wglD3DSurfaceNextY_ == nullptr);
    }

    if (d3dSurfaceNextUV_)
    {
        wglD3DSurfaceNextUV_ = wglDXRegisterObjectNV(
            wglD3DDevice_, d3dSurfaceNextUV_, textureNextUV_,
            GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
        resourceNextUVRegisteredFailed_ = (wglD3DSurfaceNextUV_ == nullptr);
    }

    clearGL();
}

void Nv12Render_Dxva2::render(const decoder_sdk::Frame &frame)
{
    if (!frame.isValid() || frame.pixelFormat() != decoder_sdk::ImageFormat::kDxva2)
    {
        clearGL();
        return;
    }

    // 获取DXVA2表面数据
    IDirect3DSurface9 *surface = reinterpret_cast<IDirect3DSurface9 *>(frame.data(0));
    if (!surface)
    {
        return;
    }

    // 在锁外进行数据复制操作
    HRESULT hr = S_OK;

    // 对于NV12格式，需要正确处理Y和UV平面
    // NV12格式：Y平面在前，UV交错在后

    // 复制Y平面数据到next表面
    if (d3dSurfaceNextY_)
    {
        // 创建源矩形，只复制Y平面部分
        RECT srcRectY = {0, 0, videoWidth_, videoHeight_};
        hr = d3dDevice_->StretchRect(surface, &srcRectY, d3dSurfaceNextY_, nullptr, D3DTEXF_NONE);
        if (FAILED(hr))
        {
            qDebug() << "Failed to copy DXVA2 Y surface";
            return;
        }
    }

    // 复制UV平面数据到next表面
    if (d3dSurfaceNextUV_)
    {
        // NV12格式的UV数据从Y平面高度开始
        RECT srcRectUV = {0, videoHeight_, videoWidth_, videoHeight_ + videoHeight_ / 2};
        hr = d3dDevice_->StretchRect(surface, &srcRectUV, d3dSurfaceNextUV_, nullptr, D3DTEXF_NONE);
        if (FAILED(hr))
        {
            qDebug() << "Failed to copy DXVA2 UV surface";
            return;
        }
    }

    // 锁定next对象进行OpenGL访问
    if (wglD3DSurfaceNextY_ && !resourceNextYRegisteredFailed_)
    {
        wglDXLockObjectsNV(wglD3DDevice_, 1, &wglD3DSurfaceNextY_);
    }
    if (wglD3DSurfaceNextUV_ && !resourceNextUVRegisteredFailed_)
    {
        wglDXLockObjectsNV(wglD3DDevice_, 1, &wglD3DSurfaceNextUV_);
    }

    // 最小锁粒度：只在交换缓冲区时加锁
    {
        QMutexLocker lock(&mtx_);
        std::swap(textureCurrentY_, textureNextY_);
        std::swap(textureCurrentUV_, textureNextUV_);

        std::swap(d3dSurfaceCurrentY_, d3dSurfaceNextY_);
        std::swap(d3dSurfaceCurrentUV_, d3dSurfaceNextUV_);

        std::swap(wglD3DSurfaceCurrentY_, wglD3DSurfaceNextY_);
        std::swap(wglD3DSurfaceCurrentUV_, wglD3DSurfaceNextUV_);

        std::swap(resourceCurrentYRegisteredFailed_, resourceNextYRegisteredFailed_);
        std::swap(resourceCurrentUVRegisteredFailed_, resourceNextUVRegisteredFailed_);
    }
}

void Nv12Render_Dxva2::draw()
{
    QMutexLocker lock(&mtx_);

    program_.bind();
    vbo_.bind();

    program_.enableAttributeArray("vertexIn");
    program_.enableAttributeArray("textureIn");
    program_.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 2, 0);
    program_.setAttributeBuffer("textureIn", GL_FLOAT, 2 * 4 * sizeof(GLfloat), 2, 0);

    // 绑定当前纹理
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureCurrentY_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textureCurrentUV_);

    program_.setUniformValue("textureY", 0);
    program_.setUniformValue("textureUV", 1);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    program_.disableAttributeArray("vertexIn");
    program_.disableAttributeArray("textureIn");
    vbo_.release();
    program_.release();

    // 解锁当前对象
    if (wglD3DSurfaceCurrentY_ && !resourceCurrentYRegisteredFailed_)
    {
        wglDXUnlockObjectsNV(wglD3DDevice_, 1, &wglD3DSurfaceCurrentY_);
    }
    if (wglD3DSurfaceCurrentUV_ && !resourceCurrentUVRegisteredFailed_)
    {
        wglDXUnlockObjectsNV(wglD3DDevice_, 1, &wglD3DSurfaceCurrentUV_);
    }
}

QOpenGLFramebufferObject *Nv12Render_Dxva2::getFrameBuffer(const QSize &size)
{
    QMutexLocker lock(&mtx_);

    QOpenGLFramebufferObject *frameBuffer = new QOpenGLFramebufferObject(size);
    frameBuffer->bind();

    draw();

    return frameBuffer;
}

void Nv12Render_Dxva2::clearGL()
{
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void Nv12Render_Dxva2::cleanupD3D9()
{
    if (d3dDevice_)
    {
        d3dDevice_->Release();
        d3dDevice_ = nullptr;
    }
    if (d3d9_)
    {
        d3d9_->Release();
        d3d9_ = nullptr;
    }
}

void Nv12Render_Dxva2::cleanupWGL()
{
    // 注销所有WGL对象
    if (wglD3DSurfaceCurrentY_)
    {
        wglDXUnregisterObjectNV(wglD3DDevice_, wglD3DSurfaceCurrentY_);
        wglD3DSurfaceCurrentY_ = nullptr;
    }
    if (wglD3DSurfaceCurrentUV_)
    {
        wglDXUnregisterObjectNV(wglD3DDevice_, wglD3DSurfaceCurrentUV_);
        wglD3DSurfaceCurrentUV_ = nullptr;
    }
    if (wglD3DSurfaceNextY_)
    {
        wglDXUnregisterObjectNV(wglD3DDevice_, wglD3DSurfaceNextY_);
        wglD3DSurfaceNextY_ = nullptr;
    }
    if (wglD3DSurfaceNextUV_)
    {
        wglDXUnregisterObjectNV(wglD3DDevice_, wglD3DSurfaceNextUV_);
        wglD3DSurfaceNextUV_ = nullptr;
    }

    if (wglD3DDevice_)
    {
        wglDXCloseDeviceNV(wglD3DDevice_);
        wglD3DDevice_ = nullptr;
    }
}