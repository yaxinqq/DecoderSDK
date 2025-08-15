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
        varying vec2 vTexCoord;
        void main() {
            gl_FragColor = texture2D(texture, vTexCoord);
        }
    )";
} // namespace

VideoRender::VideoRender() : initialized_{false}, fboDrawResourcesInitialized_{false}
{
    bufferQueue_ = std::make_unique<RenderBufferQueue>(3);
}

VideoRender::~VideoRender()
{
    // 释放当前显示buffer
    if (currentDisplayBuffer_) {
        bufferQueue_->releaseDisplayBuffer(currentDisplayBuffer_);
        currentDisplayBuffer_ = nullptr;
    }

    // 清理缓冲队列
    if (bufferQueue_) {
        bufferQueue_->cleanup();
    }

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

    // 初始化循环缓冲队列
    if (!bufferQueue_->initialize(QSize(frame->width(), frame->height()), {})) {
        qWarning() << QStringLiteral("[VideoRender] Failed to initialize buffer queue");
        return;
    }

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

    initialized_.store(true);
}

void VideoRender::render(const std::shared_ptr<decoder_sdk::Frame> &frame)
{
    if (!frame || !frame->isValid() || !isValid()) {
        return;
    }

    // 获取一个空闲的buffer用于渲染
    const auto frameDurationMs = frame->durationByFps() * 1000;
    RenderBuffer *buffer = bufferQueue_->acquireForRender(static_cast<int>(frameDurationMs * 0.5));
    if (!buffer) {
        // 没有可用buffer，丢弃此帧
        return;
    }

    // 绑定FBO并让子类渲染到其中
    buffer->fbo->bind();
    glViewport(0, 0, frame->width(), frame->height());
    const bool success = renderFrame(*frame);
    buffer->fbo->release();
    buffer->durationMs = frameDurationMs;

    if (success) {
        GLsync fence = nullptr;

        // 如果支持fence，创建fence对象进行同步
        if (supportsGlFence_) {
            fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (!fence) {
                qWarning() << QStringLiteral("[VideoRender] Failed to create fence");
            } else {
                // 进行同步等待，期间让出CPU避免忙等。等待的时间为frame duration的一半，避免sleep
                // for的精度问题
                const auto halfDurationMicros = static_cast<int64_t>(frameDurationMs * 1000 * 0.5);
                const auto sleepInterval = std::min((int64_t)1000, halfDurationMicros);
                const auto finishTimePoint = std::chrono::steady_clock::now() +
                                             std::chrono::microseconds(halfDurationMicros);
                GLenum waitResult = GL_TIMEOUT_EXPIRED;
                do {
                    // 睡眠，避免占满 CPU
                    std::this_thread::sleep_for(std::chrono::microseconds(sleepInterval));
                    // 轮询fence是否完成
                    waitResult = glClientWaitSync(fence, 0, 0);

                } while (waitResult != GL_ALREADY_SIGNALED &&
                         waitResult != GL_CONDITION_SATISFIED &&
                         std::chrono::steady_clock::now() < finishTimePoint);
            }
        } else {
            // 不支持fence时，使用glFlush确保命令提交
            glFlush();
        }

        // 标记渲染完成，不阻塞等待
        bufferQueue_->markRenderFinished(buffer, fence);
    } else {
        // 渲染失败，释放buffer
        buffer->inUse.store(false);
        qWarning() << QStringLiteral("[VideoRender] Frame render failed");
    }

    // 清理渲染资源
    cleanupRenderResources();
}

void VideoRender::draw()
{
    if (!isValid()) {
        return;
    }

    // 获得新的展示Buffer
    RenderBuffer *newDisplayBuffer = bufferQueue_->acquireForDisplay();

    if (newDisplayBuffer && newDisplayBuffer->fbo && newDisplayBuffer->fbo->isValid()) {
        // 有新的buffer可用
        if (currentDisplayBuffer_ != newDisplayBuffer) {
            // 释放旧buffer
            if (currentDisplayBuffer_) {
                bufferQueue_->releaseDisplayBuffer(currentDisplayBuffer_);
            }
            currentDisplayBuffer_ = newDisplayBuffer;
        }
    }

    // 绘制当前buffer
    if (currentDisplayBuffer_ && currentDisplayBuffer_->fbo &&
        currentDisplayBuffer_->fbo->isValid()) {
        drawFbo(currentDisplayBuffer_->fbo);
    } else {
        // 完全没有可用buffer时，清屏
        clearGL();
    }
}

QSharedPointer<QOpenGLFramebufferObject> VideoRender::getFrameBuffer()
{
    // 获取当前显示的buffer进行深拷贝
    if (!currentDisplayBuffer_ || !currentDisplayBuffer_->fbo) {
        return nullptr;
    }

    // 深拷贝当前FBO
    auto copyFbo =
        createFbo(currentDisplayBuffer_->fbo->size(), currentDisplayBuffer_->fbo->format());
    if (!copyFbo) {
        return nullptr;
    }

    // 使用blit进行深拷贝
    QOpenGLFramebufferObject::blitFramebuffer(copyFbo.get(), currentDisplayBuffer_->fbo.get());

    return copyFbo;
}

bool VideoRender::isValid() const
{
    return initialized_.load() && fboDrawResourcesInitialized_.load();
}

RenderBufferQueue::Statistics VideoRender::getStatistics() const
{
    return bufferQueue_->getStatistics();
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