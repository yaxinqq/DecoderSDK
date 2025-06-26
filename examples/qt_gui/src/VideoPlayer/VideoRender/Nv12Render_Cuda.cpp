#ifdef _WIN32
#include <Windows.h>
#endif // WIN32

#include "../Commonutils.h"
#include "Nv12Render_Cuda.h"

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

inline bool check(int e, int iLine, const char *szFile)
{
    if (e != 0) {
        const char *errstr = NULL;
        cuGetErrorString(static_cast<CUresult>(e), &errstr);
        qDebug() << "General error " << e << " error string: " << errstr << " at line " << iLine
                 << " in file " << szFile;
        return false;
    }
    return true;
}

#define ck(call) check(call, __LINE__, __FILE__)

Nv12Render_Cuda::Nv12Render_Cuda(CUcontext ctx) : context_(ctx ? ctx : CudaUtils::getCudaContext())
{
    if (!context_) {
        CUdevice cuDevice;
        ck(cuDeviceGet(&cuDevice, 0));
        ck(cuCtxCreate(&context_, CU_CTX_SCHED_BLOCKING_SYNC, cuDevice));
        needDestoryContext_ = true;
    }
}

Nv12Render_Cuda::~Nv12Render_Cuda()
{
    // qDebug() << "Nv12Render_Cuda::~Nv12Render_Cuda() in";
    if (!resourceCurrentYRegisteredFailed_) {
        ck(cuGraphicsUnmapResources(1, &resourceCurrentY_, copyYStream_));
        ck(cuGraphicsUnregisterResource(resourceCurrentY_));
    }
    if (!resourceCurrentUVRegisteredFailed_) {
        ck(cuGraphicsUnmapResources(1, &resourceCurrentUV_, copyUVStream_));
        ck(cuGraphicsUnregisterResource(resourceCurrentUV_));
    }
    if (!resourceNextYRegisteredFailed_) {
        ck(cuGraphicsUnmapResources(1, &resourceNextY_, copyYStream_));
        (cuGraphicsUnregisterResource(resourceNextY_));
    }
    if (!resourceNextUVRegisteredFailed_) {
        ck(cuGraphicsUnmapResources(1, &resourceNextUV_, copyUVStream_));
        ck(cuGraphicsUnregisterResource(resourceNextUV_));
    }

    if (copyYStream_)
        ck(cuStreamDestroy(copyYStream_));
    if (copyUVStream_)
        ck(cuStreamDestroy(copyUVStream_));

    if (needDestoryContext_) {
        ck(cuCtxDestroy(context_));
    }

    vbo_.destroy();
    for (auto *id : {&idCurrentY_, &idCurrentUV_, &idNextY_, &idNextUV_}) {
        glDeleteTextures(1, id);
    }

    // qDebug() << "Nv12Render_Cuda::~Nv12Render_Cuda() out";
}

Q_GLOBAL_STATIC(QMutex, initMutex)
void Nv12Render_Cuda::initialize(const int width, const int height, const bool horizontal,
                                 const bool vertical)
{
    initializeOpenGLFunctions();
    QMutexLocker initLock(initMutex());

    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vsrc);
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fsrc);
    program_.link();

    if (horizontal) {
        if (vertical) {
            GLfloat points[]{
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
                1.0f,
                1.0f,
                0.0f,
                1.0f,
                1.0f,
                0.0f,
                0.0f,
                0.0f,
            };

            vbo_.create();
            vbo_.bind();
            vbo_.allocate(points, sizeof(points));
        } else {
            GLfloat points[]{
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
                1.0f,
                0.0f,
                0.0f,
                0.0f,
                1.0f,
                1.0f,
                0.0f,
                1.0f,
            };

            vbo_.create();
            vbo_.bind();
            vbo_.allocate(points, sizeof(points));
        }
    } else {
        if (vertical) {
            GLfloat points[]{
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
                0.0f,
                1.0f,
                1.0f,
                1.0f,
                0.0f,
                0.0f,
                1.0f,
                0.0f,
            };

            vbo_.create();
            vbo_.bind();
            vbo_.allocate(points, sizeof(points));
        } else {
            GLfloat points[]{
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
                0.0f,
                0.0f,
                1.0f,
                0.0f,
                0.0f,
                1.0f,
                1.0f,
                1.0f,
            };

            vbo_.create();
            vbo_.bind();
            vbo_.allocate(points, sizeof(points));
        }
    }

    /*glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(false);*/

    // 纹理和缓冲的初始值
    const qopengl_GLsizeiptr frameSize =
        static_cast<qopengl_GLsizeiptr>(width) * static_cast<qopengl_GLsizeiptr>(height);
    const std::vector<unsigned char> initYData(frameSize, 0);
    const std::vector<unsigned char> initUVData(frameSize / 2, 128);

    // 初始化纹理
    for (auto *id : {&idCurrentY_, &idNextY_}) {
        glGenTextures(1, id);
        glBindTexture(GL_TEXTURE_2D, *id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE,
                     initYData.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    for (auto *id : {&idCurrentUV_, &idNextUV_}) {
        glGenTextures(1, id);
        glBindTexture(GL_TEXTURE_2D, *id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG, width >> 1, height >> 1, 0, GL_RG, GL_UNSIGNED_BYTE,
                     initUVData.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    clearGL();

    // 初始化CUDA部分，映射纹理，创建流
    ck(cuCtxSetCurrent(context_));

    if (cuGraphicsGLRegisterImage(&resourceCurrentY_, idCurrentY_, GL_TEXTURE_2D,
                                  CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD) != CUDA_SUCCESS)
        resourceCurrentYRegisteredFailed_ = true;
    if (cuGraphicsGLRegisterImage(&resourceCurrentUV_, idCurrentUV_, GL_TEXTURE_2D,
                                  CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD) != CUDA_SUCCESS)
        resourceCurrentUVRegisteredFailed_ = true;

    if (cuGraphicsGLRegisterImage(&resourceNextY_, idNextY_, GL_TEXTURE_2D,
                                  CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD) != CUDA_SUCCESS)
        resourceNextYRegisteredFailed_ = true;
    if (cuGraphicsGLRegisterImage(&resourceNextUV_, idNextUV_, GL_TEXTURE_2D,
                                  CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD) != CUDA_SUCCESS)
        resourceNextUVRegisteredFailed_ = true;

    ck(cuStreamCreate(&copyYStream_, CU_STREAM_NON_BLOCKING));
    ck(cuStreamCreate(&copyUVStream_, CU_STREAM_NON_BLOCKING));

    // 资源映射，只映射一次，不再重复映射
    if (!resourceCurrentYRegisteredFailed_) {
        ck(cuGraphicsMapResources(1, &resourceCurrentY_, 0));
        ck(cuGraphicsSubResourceGetMappedArray(&cudaArrayCurrentY_, resourceCurrentY_, 0, 0));
    }

    if (!resourceCurrentUVRegisteredFailed_) {
        ck(cuGraphicsMapResources(1, &resourceCurrentUV_, 0));
        ck(cuGraphicsSubResourceGetMappedArray(&cudaArrayCurrentUV_, resourceCurrentUV_, 0, 0));
    }

    if (!resourceNextYRegisteredFailed_) {
        ck(cuGraphicsMapResources(1, &resourceNextY_, 0));
        ck(cuGraphicsSubResourceGetMappedArray(&cudaArrayNextY_, resourceNextY_, 0, 0));
    }

    if (!resourceNextUVRegisteredFailed_) {
        ck(cuGraphicsMapResources(1, &resourceNextUV_, 0));
        ck(cuGraphicsSubResourceGetMappedArray(&cudaArrayNextUV_, resourceNextUV_, 0, 0));
    }
}

void Nv12Render_Cuda::render(const decoder_sdk::Frame &frame)
{
    if (resourceCurrentYRegisteredFailed_ || resourceCurrentUVRegisteredFailed_ ||
        resourceNextYRegisteredFailed_ || resourceNextUVRegisteredFailed_ || !copyYStream_ ||
        !copyUVStream_)
        return;

    if (!frame.isValid()) {
        clearGL();
        return;
    }

    // Y通道处理
    CUDA_MEMCPY2D mY = {0};
    mY.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    mY.srcDevice = reinterpret_cast<CUdeviceptr>(frame.data(0));
    mY.srcPitch = frame.linesize(0);
    mY.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    mY.dstArray = cudaArrayNextY_;
    mY.WidthInBytes = frame.width();
    mY.Height = frame.height();
    ck(cuMemcpy2DAsync(&mY, copyYStream_));

    // UV 通道处理
    CUDA_MEMCPY2D mUV = {0};
    mUV.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    mUV.srcDevice = reinterpret_cast<CUdeviceptr>(frame.data(1));
    mUV.srcPitch = frame.linesize(1);
    mUV.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    mUV.dstArray = cudaArrayNextUV_;
    mUV.WidthInBytes = frame.width();
    mUV.Height = frame.height() >> 1;
    ck(cuMemcpy2DAsync(&mUV, copyUVStream_));

    // 添加回调，等异步任务结束后，解除条件变量的等待
    ck(cuStreamAddCallback(
        copyYStream_,
        [](CUstream stream, CUresult result, void *userData) {
            Nv12Render_Cuda *self = static_cast<Nv12Render_Cuda *>(userData);
            if (!self)
                return;

            // 结束条件变量的等待
            {
                std::lock_guard<std::mutex> lock(self->conditionalMtx_);
                self->copyYSucced_.store(true);
                self->conditional_.notify_all();
            }
        },
        this, 0));
    ck(cuStreamAddCallback(
        copyUVStream_,
        [](CUstream stream, CUresult result, void *userData) {
            Nv12Render_Cuda *self = static_cast<Nv12Render_Cuda *>(userData);
            if (!self)
                return;

            // 结束条件变量的等待
            {
                std::lock_guard<std::mutex> lock(self->conditionalMtx_);
                self->copyUVSucced_.store(true);
                self->conditional_.notify_all();
            }
        },
        this, 0));

    // 等待事件通知
    {
        std::unique_lock<std::mutex> l(conditionalMtx_);
        conditional_.wait(l, [this]() { return copyYSucced_.load() && copyUVSucced_.load(); });
    }

    // 重置事件状态
    copyYSucced_.store(false);
    copyUVSucced_.store(false);

    {
        // 调整锁的颗粒度，主要的数据交互集中在下面
        QMutexLocker lock(&mtx_);
        std::swap(idCurrentY_, idNextY_);
        std::swap(idCurrentUV_, idNextUV_);

        std::swap(resourceCurrentY_, resourceNextY_);
        std::swap(resourceCurrentUV_, resourceNextUV_);

        std::swap(cudaArrayCurrentY_, cudaArrayNextY_);
        std::swap(cudaArrayCurrentUV_, cudaArrayNextUV_);
    }
}

void Nv12Render_Cuda::draw()
{
    QMutexLocker lock(&mtx_);
    // clearGL();    // 暂时不开启，开启会影响GraphicsView和GraphicsItem

    program_.bind();
    vbo_.bind();
    program_.enableAttributeArray("vertexIn");
    program_.enableAttributeArray("textureIn");
    program_.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 2, 0);
    program_.setAttributeBuffer("textureIn", GL_FLOAT, 2 * 4 * sizeof(GLfloat), 2, 0);

    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, idCurrentY_);

    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, idCurrentUV_);

    program_.setUniformValue("textureY", 1);
    program_.setUniformValue("textureUV", 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    program_.disableAttributeArray("vertexIn");
    program_.disableAttributeArray("textureIn");
    vbo_.release();
    program_.release();
}

QOpenGLFramebufferObject *Nv12Render_Cuda::getFrameBuffer(const QSize &size)
{
    QMutexLocker lock(&mtx_);
    // clearGL();   // 暂时不开启，开启会影响GraphicsView和GraphicsItem

    QOpenGLFramebufferObject *frameBuffer = new QOpenGLFramebufferObject(size);
    frameBuffer->bind();

    program_.bind();
    vbo_.bind();
    program_.enableAttributeArray("vertexIn");
    program_.enableAttributeArray("textureIn");
    program_.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 2, 0);
    program_.setAttributeBuffer("textureIn", GL_FLOAT, 2 * 4 * sizeof(GLfloat), 2, 0);

    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, idCurrentY_);

    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, idCurrentUV_);

    program_.setUniformValue("textureY", 1);
    program_.setUniformValue("textureUV", 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    program_.disableAttributeArray("vertexIn");
    program_.disableAttributeArray("textureIn");
    vbo_.release();
    program_.release();

    return frameBuffer;
}

void Nv12Render_Cuda::clearGL()
{
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

VideoRender *createRender(void *ctx)
{
    return new Nv12Render_Cuda((CUcontext)ctx);
}
