#include "VideoRender.h"

#include <QDateTime>
#include <QOpenGLContext>
#include <QThread>

namespace {
const char *vsrc = R"(
    #ifdef GL_ES
        precision mediump float;
    #endif

        attribute vec4 position;
        attribute vec2 texCoord;
        varying vec2 vTexCoord;
        void main() {
            gl_Position = position;
            vTexCoord = texCoord;
        }
    )";

const char *fsrc = R"(
    #ifdef GL_ES
        precision mediump float;
    #endif

        uniform sampler2D texture;
        uniform vec4 zoomRect;
        varying vec2 vTexCoord;
        void main() {
            vec2 zoomOrigin = zoomRect.xy;
            vec2 zoomSize   = zoomRect.zw;
            vec2 zoomTexCoord = zoomOrigin + vTexCoord * zoomSize;

            gl_FragColor = texture2D(texture, zoomTexCoord);
        }
    )";
} // namespace

VideoRender::VideoRender()
    : initialized_{false}, fboDrawResourcesInitialized_{false}, digitalZoomRect_{0.0, 0.0, 1.0, 1.0}
{
    forceGpuFinish_ = false;
}

VideoRender::~VideoRender()
{
    curFbo_.reset();
    nextFbo_.reset();
    fboDrawVbo_.destroy();
}

void VideoRender::initialize(const std::shared_ptr<decoder_sdk::Frame> &frame,
                             const bool horizontal, const bool vertical)
{
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (initialized_.load() || !frame || !frame->isValid() || !context) {
        return;
    }

    initializeOpenGLFunctions();

    // 如果配置文件中没有明确需要glFinish，则根据显卡型号，去开启，目前发现Intel集显总是需要开启的
    const QString vendor(reinterpret_cast<const char *>(glGetString(GL_VENDOR)));
    isIntelGpu_ = vendor.compare(QStringLiteral("Intel"), Qt::CaseInsensitive) == 0;

    // 初始化FBO
    const QSize fboSize(frame->width(), frame->height());
    curFbo_ = createFbo(fboSize, {});
    nextFbo_ = createFbo(fboSize, {});

    // 初始化FBO绘制资源
    if (!fboDrawResourcesInitialized_.load()) {
        fboDrawResourcesInitialized_ =
            initializeFboDrawResources(QSize(frame->width(), frame->height()));
    }
    if (!fboDrawResourcesInitialized_.load())
        return;

    // 调用子类的初始化方法
    if (!initRenderVbo(horizontal, vertical))
        return;

    if (!initRenderShader(*frame))
        return;

    if (!initRenderTexture(*frame))
        return;

    if (!initInteropsResource(*frame))
        return;

    // 查询是否支持glFence
    supportsGlFence_ = context->hasExtension(QByteArrayLiteral("GL_ARB_sync")) ||
                       context->hasExtension(QByteArrayLiteral("GL_OES_EGL_sync"));
    qInfo() << QStringLiteral("[VideoRender] Support glFence: %1")
                   .arg(supportsGlFence_ ? QStringLiteral("true") : QStringLiteral("false"));

    initialized_.store(true);
}

void VideoRender::render(const std::shared_ptr<decoder_sdk::Frame> &frame)
{
    if (!frame || !frame->isValid() || !isValid()) {
        return;
    }

    // 绑定FBO并让子类渲染到其中
    nextFbo_->bind();
    glViewport(0, 0, frame->width(), frame->height());
    const bool success = renderFrame(*frame);
    nextFbo_->release();

    if (forceGpuFinish_ || isIntelGpu_) {
        glFinish();
    } else {
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        // 等待GPU侧完成
        // const auto startTime = std::chrono::steady_clock::now();
        const auto waitResult =
            glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        if (waitResult == GL_WAIT_FAILED || waitResult == GL_TIMEOUT_EXPIRED) {
            qWarning() << QStringLiteral("[VideoRender] glClientWaitSync failed!");
        }
        // const auto endTime = std::chrono::steady_clock::now();
        // qInfo() << QStringLiteral("[VideoRender] glClientWaitSync cost time: ") <<
        // std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count() <<
        // "ns";
        glDeleteSync(fence);
    }

    // 清理渲染资源
    cleanupRenderResources();

    {
        QMutexLocker lock(&mtx_);
        std::swap(curFbo_, nextFbo_);
    }
}

void VideoRender::draw()
{
    if (!isValid()) {
        return;
    }

    QMutexLocker lock(&mtx_);
    drawFbo(curFbo_);
}

QSharedPointer<QOpenGLFramebufferObject> VideoRender::getFrameBuffer()
{
    QMutexLocker lock(&mtx_);

    // 深拷贝当前FBO
    auto copyFbo = createFbo(curFbo_->size(), curFbo_->format());
    if (!copyFbo) {
        return nullptr;
    }

    // 使用blit进行深拷贝
    QOpenGLFramebufferObject::blitFramebuffer(copyFbo.get(), curFbo_.get());

    return copyFbo;
}

bool VideoRender::isValid() const
{
    return initialized_.load() && fboDrawResourcesInitialized_.load();
}

bool VideoRender::shouldRebuild() const
{
    return false;
}

void VideoRender::setDigitalZoomRect(const QRectF &rect)
{
    if (rect == digitalZoomRect_)
        return;

    digitalZoomRect_ = rect;
    fboDrawProgram_.bind();
    fboDrawProgram_.setUniformValue("zoomRect", transToOpenGLUniform(digitalZoomRect_));
    fboDrawProgram_.release();
}

QRectF VideoRender::digitalZoomRect() const
{
    return digitalZoomRect_;
}

void VideoRender::initDefaultVBO(QOpenGLBuffer &vbo, const bool horizontal,
                                 const bool vertical) const
{
    // 设置顶点数据
    const GLfloat points[] = {
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

    vbo.create();
    vbo.bind();
    vbo.allocate(points, sizeof(points));
    vbo.release();
}

void VideoRender::clearGL()
{
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // 改为不透明黑色
    glClear(GL_COLOR_BUFFER_BIT);
}

bool VideoRender::initializeFboDrawResources(const QSize &size)
{
    fboDrawProgram_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vsrc);
    fboDrawProgram_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fsrc);
    fboDrawProgram_.link();
    fboDrawProgram_.bind();
    fboDrawProgram_.setUniformValue("texture", 0);
    fboDrawProgram_.setUniformValue("zoomRect", transToOpenGLUniform(digitalZoomRect_));
    fboDrawProgram_.release();

    // 全屏quad的顶点数据（交错式布局）
    const GLfloat vertices[] = {// 位置坐标               // 纹理坐标
                                -1.0f, 1.0f,  0.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f,
                                -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f};

    fboDrawVbo_.create();
    fboDrawVbo_.bind();
    fboDrawVbo_.allocate(vertices, sizeof(vertices));
    fboDrawVbo_.release();

    return true;
}

void VideoRender::drawFbo(QSharedPointer<QOpenGLFramebufferObject> fbo)
{
    if (!fbo || !fboDrawResourcesInitialized_.load()) {
        return;
    }

    fboDrawProgram_.bind();
    fboDrawVbo_.bind();

    fboDrawProgram_.enableAttributeArray("position");
    fboDrawProgram_.enableAttributeArray("texCoord");
    fboDrawProgram_.setAttributeBuffer("position", GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
    fboDrawProgram_.setAttributeBuffer("texCoord", GL_FLOAT, 2 * sizeof(GLfloat), 2,
                                       4 * sizeof(GLfloat));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fbo->texture());

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    fboDrawProgram_.disableAttributeArray("position");
    fboDrawProgram_.disableAttributeArray("texCoord");
    fboDrawVbo_.release();
    fboDrawProgram_.release();
}

QSharedPointer<QOpenGLFramebufferObject> VideoRender::createFbo(
    const QSize &size, const QOpenGLFramebufferObjectFormat &fmt)
{
    if (!size.isValid()) {
        return nullptr;
    }
    return QSharedPointer<QOpenGLFramebufferObject>::create(size, fmt);
}

QVector4D VideoRender::transToOpenGLUniform(const QRectF &rect, bool needTrans) const
{
    return QVector4D(rect.x(), needTrans ? 1 - rect.y() - rect.height() : rect.y(), rect.width(),
                     rect.height());
}