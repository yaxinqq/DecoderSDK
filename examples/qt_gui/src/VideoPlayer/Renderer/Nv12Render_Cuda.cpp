#include "Nv12Render_Cuda.h"

#ifdef CUDA_AVAILABLE
#ifdef _WIN32
#include <Windows.h>
#endif // WIN32

#include "../Base/CommonUtils.h"

namespace {
    const char *vsrc = R"(
#ifdef GL_ES
    precision highp float;
#endif
	
    attribute vec4 vertexIn;
	attribute vec2 textureIn;
	varying highp vec2 textureOut;
	void main(void)
	{
		gl_Position = vertexIn;
		textureOut = textureIn;
	}
)";

    const char *fsrc = R"(
#ifdef GL_ES
    precision highp float;
#endif

    uniform sampler2D textureY;
    uniform sampler2D textureUV;
    varying highp vec2 textureOut;

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

inline bool check(CUresult e, int iLine, const char *szFile)
{
    if (e != CUDA_SUCCESS) {
        const char *errstr = nullptr;
        cuda_utils::cuGetErrorString(e, &errstr);
        qDebug() << QStringLiteral("General error %1 error string: %2 at line %3 in file %4")
                        .arg(e)
                        .arg(errstr ? errstr : "unknown")
                        .arg(iLine)
                        .arg(szFile);
        return false;
    }
    return true;
}

#define ck(call) check(call, __LINE__, __FILE__)

Nv12Render_Cuda::Nv12Render_Cuda()
    : VideoRender()
    , context_(cuda_utils::getCudaContext())
{
    cuda_utils::cuCtxSetCurrent(context_);
}

Nv12Render_Cuda::~Nv12Render_Cuda()
{
    cleanupCudaResource();
    if (context_) {
        ck(cuda_utils::cuDevicePrimaryCtxRelease(cuda_utils::getCudaDevice()));
    }

    cleanupOpenGLResource();
}

QString Nv12Render_Cuda::renderName() const
{
    return QStringLiteral("Cuda Interop To OpenGL Render");
}

bool Nv12Render_Cuda::initRenderVbo(const bool horizontal, const bool vertical)
{
    initDefaultVBO(vbo_, horizontal, vertical);
    return true;
}

bool Nv12Render_Cuda::initRenderShader(const decoder_sdk::Frame &frame)
{
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vsrc);
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fsrc);
    program_.link();

    return true;
}

bool Nv12Render_Cuda::initRenderTexture(const decoder_sdk::Frame &frame)
{
    // 纹理和缓冲的初始值
    const auto width = frame.width();
    const auto height = frame.height();
    const qopengl_GLsizeiptr frameSize =
        static_cast<qopengl_GLsizeiptr>(width) * static_cast<qopengl_GLsizeiptr>(height);
    const std::vector<unsigned char> initYData(frameSize, 0);
    const std::vector<unsigned char> initUVData(frameSize / 2, 128);

    // 初始化纹理 Y
    glGenTextures(1, &idY_);
    glBindTexture(GL_TEXTURE_2D, idY_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE,
                 initYData.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // 初始化纹理 UV
    glGenTextures(1, &idUV_);
    glBindTexture(GL_TEXTURE_2D, idUV_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG, width >> 1, height >> 1, 0, GL_RG, GL_UNSIGNED_BYTE,
                 initUVData.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

bool Nv12Render_Cuda::initInteropsResource(const decoder_sdk::Frame &frame)
{
    Q_UNUSED(frame);

    if (!ck(cuda_utils::cuGraphicsGLRegisterImage(&resourceY_, idY_, GL_TEXTURE_2D,
                                                  CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD))) {
        cleanupCudaResource();
        return false;
    }
    if (!ck(cuda_utils::cuGraphicsGLRegisterImage(&resourceUV_, idUV_, GL_TEXTURE_2D,
                                                  CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD))) {
        cleanupCudaResource();
        return false;
    }

    if (!ck(cuda_utils::cuStreamCreate(&copyStream_, 1))) {
        cleanupCudaResource();
        return false;
    }

    return true;
}

bool Nv12Render_Cuda::interopToOpenGL(const decoder_sdk::Frame &frame)
{
    if (!resourceY_ || !resourceUV_ || !copyStream_)
        return false;

    if (!frame.isValid()) {
        return false;
    }

    // const auto startTime = std::chrono::steady_clock::now();

    // 资源映射
    if (!mapCudaResource()) {
        return false;
    }

    // Y通道处理
    CUDA_MEMCPY2D mY = { 0 };
    mY.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    mY.srcDevice = reinterpret_cast<CUdeviceptr>(frame.data(0));
    mY.srcPitch = frame.linesize(0);
    mY.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    mY.dstArray = cudaArrayY_;
    mY.WidthInBytes = frame.width();
    mY.Height = frame.height();
    if (!ck(cuda_utils::cuMemcpy2DAsync(&mY, copyStream_))) {
        unmapCudaResource();
        return false;
    }

    // UV 通道处理
    CUDA_MEMCPY2D mUV = { 0 };
    mUV.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    mUV.srcDevice = reinterpret_cast<CUdeviceptr>(frame.data(1));
    mUV.srcPitch = frame.linesize(1);
    mUV.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    mUV.dstArray = cudaArrayUV_;
    mUV.WidthInBytes = frame.width();
    mUV.Height = frame.height() >> 1;
    if (!ck(cuda_utils::cuMemcpy2DAsync(&mUV, copyStream_))) {
        unmapCudaResource();
        return false;
    }

    // 显式同步，暂时注释。目前来看，似乎仅unmap就能保证资源的可用及完整
    // cuda_utils::cuStreamSynchronize(copyStream_);

    // 取消映射
    if (!unmapCudaResource()) {
        return false;
    }

    // const auto endTime = std::chrono::steady_clock::now();
    // qInfo() << "!!!!!!!!!!!!!!!" << std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    
    return true;
}

bool Nv12Render_Cuda::renderToFbo(const decoder_sdk::Frame &frame)
{
    Q_UNUSED(frame);

    // 绘制
    drawFrame(idY_, idUV_);
    return true;
}

void Nv12Render_Cuda::cleanupAllResources()
{
    cleanupCudaResource();
    cleanupOpenGLResource();
}

void Nv12Render_Cuda::drawFrame(GLuint idY, GLuint idUV)
{
    program_.bind();
    vbo_.bind();
    program_.enableAttributeArray("vertexIn");
    program_.enableAttributeArray("textureIn");
    program_.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 2, 0);
    program_.setAttributeBuffer("textureIn", GL_FLOAT, 2 * 4 * sizeof(GLfloat), 2, 0);

    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, idY);

    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, idUV);

    program_.setUniformValue("textureY", 1);
    program_.setUniformValue("textureUV", 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    program_.disableAttributeArray("vertexIn");
    program_.disableAttributeArray("textureIn");
    vbo_.release();
    program_.release();
}

void Nv12Render_Cuda::cleanupCudaResource()
{
    // 销毁
    if (resourceY_) {
        if (resourceYMapped_) {
            ck(cuda_utils::cuGraphicsUnmapResources(1, &resourceY_, copyStream_));
            resourceYMapped_ = false;
        }
        ck(cuda_utils::cuGraphicsUnregisterResource(resourceY_));
        resourceY_ = nullptr;
    }
    if (resourceUV_) {
        if (resourceUVMapped_) {
            ck(cuda_utils::cuGraphicsUnmapResources(1, &resourceUV_, copyStream_));
            resourceUVMapped_ = false;
        }
        ck(cuda_utils::cuGraphicsUnregisterResource(resourceUV_));
        resourceUV_ = nullptr;
    }
    cudaArrayY_ = nullptr;
    cudaArrayUV_ = nullptr;

    if (copyStream_) {
        ck(cuda_utils::cuStreamDestroy(copyStream_));
        copyStream_ = nullptr;
    }
}

void Nv12Render_Cuda::cleanupOpenGLResource()
{
    vbo_.destroy();
    for (auto *id : { &idY_, &idUV_ }) {
        glDeleteTextures(1, id);
        *id = 0;
    }
    program_.removeAllShaders();
}

bool Nv12Render_Cuda::mapCudaResource()
{
    // 资源映射
    if (!ck(cuda_utils::cuGraphicsMapResources(1, &resourceY_, copyStream_))) {
        unmapCudaResource();
        return false;
    }
    resourceYMapped_ = true;
    if (!ck(cuda_utils::cuGraphicsSubResourceGetMappedArray(&cudaArrayY_, resourceY_, 0, 0))) {
        unmapCudaResource();
        return false;
    }

    if (!ck(cuda_utils::cuGraphicsMapResources(1, &resourceUV_, copyStream_))) {
        unmapCudaResource();
        return false;
    }
    resourceUVMapped_ = true;
    if (!ck(cuda_utils::cuGraphicsSubResourceGetMappedArray(&cudaArrayUV_, resourceUV_, 0, 0))) {
        unmapCudaResource();
        return false;
    }

    return true;
}

bool Nv12Render_Cuda::unmapCudaResource()
{
    // 取消映射
    bool retY = true;
    bool retUV = true;
    if (resourceYMapped_ && resourceY_) {
        if (ck(cuda_utils::cuGraphicsUnmapResources(1, &resourceY_, copyStream_))) {
            resourceYMapped_ = false;
        } else {
            retY = false;
        }
    }

    if (resourceUVMapped_ && resourceUV_) {
        if (ck(cuda_utils::cuGraphicsUnmapResources(1, &resourceUV_, copyStream_))) {
            resourceUVMapped_ = false;
        } else {
            retUV = false;
        }
    }

    return retY && retUV;
}

#endif
