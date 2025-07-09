#include "VideoRender.h"

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
        varying vec2 vTexCoord;
        void main() {
            gl_FragColor = texture2D(texture, vTexCoord);
        }
    )";
} // namespace

VideoRender::VideoRender() : initialized_{false}, fboDrawResourcesInitialized_{false}
{
}

VideoRender::~VideoRender()
{
    curFbo_.reset();
    nextFbo_.reset();

    fboDrawVbo_.destroy();
}

void VideoRender::initialize(const decoder_sdk::Frame &frame, const bool horizontal,
                             const bool vertical)
{
    if (initialized_.load() || !frame.isValid()) {
        return;
    }

    initializeOpenGLFunctions();

    // 初始化FBO绘制资源
    if (!fboDrawResourcesInitialized_.load()) {
        fboDrawResourcesInitialized_ =
            initializeFboDrawResources(QSize(frame.width(), frame.height()));
    }
    if (!fboDrawResourcesInitialized_.load())
        return;

    // 调用子类的初始化方法
    if (!initRenderVbo(horizontal, vertical))
        return;

    if (!initRenderShader(frame))
        return;

    if (!initRenderTexture(frame))
        return;

    if (!initInteropsResource(frame))
        return;

    initialized_.store(true);
}

void VideoRender::render(const decoder_sdk::Frame &frame)
{
    if (!frame.isValid() || !isValid()) {
        return;
    }

    // 绑定FBO并让子类渲染到其中
    nextFbo_->bind();
    glViewport(0, 0, frame.width(), frame.height());
    const bool success = renderFrame(frame);
    nextFbo_->release();

    // 更新当前FBO
    if (success) {
        // 检查是否支持fence同步（OpenGL 3.2+）
        QOpenGLContext *context = QOpenGLContext::currentContext();
        if (context && (context->hasExtension(QByteArrayLiteral("GL_ARB_sync")) || context->hasExtension("GL_OES_EGL_sync"))) {
            // 插入fence，异步等待渲染完成
            GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

            // 等待fence完成（可以设置超时）
            const GLenum result = glClientWaitSync(
                fence, GL_SYNC_FLUSH_COMMANDS_BIT,
                frame.durationByFps() * 1000 * 1000 *
                    1000); // 超时时间根据帧的持续时间进行设置 durationByFps(单位 s)
            glDeleteSync(fence);

            if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {
                QMutexLocker l(&mutex_);
                std::swap(curFbo_, nextFbo_);
            }
        } else {
            // 退化策略：使用glFinish确保渲染完成
            glFinish();
            QMutexLocker l(&mutex_);
            std::swap(curFbo_, nextFbo_);
        }
    }
    cleanupRenderResources();
}

void VideoRender::draw()
{
    if (!isValid()) {
        return;
    }

    QMutexLocker l(&mutex_);
    if (curFbo_ && curFbo_->isValid()) {
        drawFbo(curFbo_);
    }
}

QSharedPointer<QOpenGLFramebufferObject> VideoRender::getFrameBuffer()
{
    if (!curFbo_) {
        return nullptr;
    }

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

bool VideoRender::initializeFboDrawResources(const QSize &size)
{
    fboDrawProgram_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vsrc);
    fboDrawProgram_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fsrc);
    fboDrawProgram_.link();

    // 全屏quad的顶点数据（交错式布局）
    const GLfloat vertices[] = {// 位置坐标               // 纹理坐标
                                -1.0f, 1.0f,  0.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f,
                                -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f};

    fboDrawVbo_.create();
    fboDrawVbo_.bind();
    fboDrawVbo_.allocate(vertices, sizeof(vertices));
    fboDrawVbo_.release();

    // 创建FBO
    curFbo_ = createFbo(size, {});
    nextFbo_ = createFbo(size, {});

    return !curFbo_.isNull() && !nextFbo_.isNull();
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
    fboDrawProgram_.setUniformValue("texture", 0);

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

void VideoRender::clearGL()
{
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}
