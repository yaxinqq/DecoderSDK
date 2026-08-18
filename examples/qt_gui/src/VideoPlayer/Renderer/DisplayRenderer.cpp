#include "DisplayRenderer.h"

#include <QDebug>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QThread>

namespace {
    // 顶点着色器
    const char *vsrc = R"(
    #ifdef GL_ES
        precision highp float;
    #endif

        attribute vec4 position;
        attribute vec2 texCoord;
        varying highp vec2 vTexCoord;
        
        /*
         * texFlip.x : horizontal flip (0 or 1)
         * texFlip.y : vertical flip   (0 or 1)
         */
        uniform vec2 texFlip;

        vec2 applyFlip(vec2 uv)
        {
            return mix(uv, 1.0 - uv, texFlip);
        }

        void main() {
            gl_Position = position;

            vec2 uv = texCoord;
            uv = applyFlip(uv);
            vTexCoord = uv;            
        }
    )";

    // 片段着色器
    // 这里可以无脑指定 highp，因为QOpenGLShaderProgram以通过GL_FRAGMENT_PRECISION_HIGH宏来处理了精度
    // 如果当前不支持highp，会自动降级为mediump #define highp mediump
    const char *fsrc = R"(
    #ifdef GL_ES
        precision highp float;
    #endif

        uniform sampler2D texture;
        uniform vec4 zoomRect;
        uniform vec2 texFlip;
        uniform float brightness;
        uniform float contrast;
        uniform float saturation;
        uniform float hue;
        varying highp vec2 vTexCoord;
        void main() {
            const float kEps = 1.0e-6;
            const vec3 kLumaWeights = vec3(0.2126, 0.7152, 0.0722);

            bool hasBrightness = abs(brightness) > kEps;
            bool hasContrast = abs(contrast - 1.0) > kEps;
            bool hasSaturation = abs(saturation - 1.0) > kEps;
            bool hasHue = abs(hue) > kEps;

            vec2 zoomOrigin = mix(zoomRect.xy, vec2(1.0) - zoomRect.xy - zoomRect.zw, texFlip);
            vec2 zoomSize = zoomRect.zw;
            vec2 zoomTexCoord = zoomOrigin + vTexCoord * zoomSize;
            vec4 source = texture2D(texture, zoomTexCoord);

            if (!hasBrightness && !hasContrast && !hasSaturation && !hasHue) {
                gl_FragColor = source;
                return;
            }

            vec3 color = source.rgb;
            if (hasBrightness) {
                color += vec3(brightness);
            }
 
            float originLuma = 0.0;       
            if (hasContrast || hasSaturation || hasHue) {
                originLuma = dot(color, kLumaWeights);
            }

            if (hasContrast) {
                float luma = originLuma;
                vec3 chroma = color - vec3(luma);
                luma *= contrast;
                color = vec3(luma) + chroma;
            }

            if (hasSaturation) {
                float gray = originLuma;
                color = mix(vec3(gray), color, saturation);
            }
            if (hasHue) {
                float y = originLuma;
                float u = dot(color, vec3(-0.114572, -0.385428, 0.5));
                float v = dot(color, vec3(0.5, -0.454153, -0.045847));
                float angle = hue * 6.28318530718;
                float cosA = cos(angle);
                float sinA = sin(angle);
                float newU = u * cosA - v * sinA;
                float newV = u * sinA + v * cosA;
                color = vec3(y + 1.5748 * newV, y - 0.187324 * newU - 0.468124 * newV,
                             y + 1.8556 * newU);
            }

            gl_FragColor = vec4(clamp(color, 0.0, 1.0), source.a);
        }
    )";
} // namespace

DisplayRenderer::DisplayRenderer(const QString &uuid)
    : fboDrawResourcesInitialized_{ false }
    , uuid_{ uuid }
{
}

DisplayRenderer::~DisplayRenderer()
{
    cleanup();
}

QString DisplayRenderer::id() const
{
    return uuid_;
}

void DisplayRenderer::initialize()
{
    initializeOpenGLFunctions();

    auto failAndRollback = [&](const QString &reason) {
        qWarning() << QStringLiteral("[DisplayRenderer] Initialize failed: %1, uninitialize can reset init state!").arg(reason);

        {
            QMutexLocker lock(&mtx_);
            nextFbo_.reset();
            curFbo_.reset();
        }

        fboDrawProgram_.removeAllShaders();
        if (fboDrawVbo_.isCreated()) {
            fboDrawVbo_.destroy();
        }
        fboDrawResourcesInitialized_.store(false);
    };

    // 初始化FBO绘制资源
    if (!fboDrawResourcesInitialized_.load()) {
        fboDrawResourcesInitialized_ =
            initializeFboDrawResources();
    }
    if (!fboDrawResourcesInitialized_.load()) {
        failAndRollback(QStringLiteral("failed to initialize FBO draw resources"));
        return;
    }
}

bool DisplayRenderer::isInitialized() const
{
    return fboDrawResourcesInitialized_.load();
}

void DisplayRenderer::cleanup()
{
    QMutexLocker lock(&mtx_);
    if (!curFbo_ && !nextFbo_ && !fboDrawVbo_.isCreated()) {
        return;
    }

    nextFbo_.reset();
    curFbo_.reset();
    fboDrawVbo_.destroy();
}

void DisplayRenderer::draw(const Stream::VideoProcessParam &param)
{
    if (!fboDrawResourcesInitialized_.load()) {
        return;
    }

    QMutexLocker lock(&mtx_);
    drawFbo(curFbo_, param);
}

QSharedPointer<QOpenGLFramebufferObject> DisplayRenderer::getFrameBuffer(const Stream::VideoProcessParam &param)
{
    QMutexLocker lock(&mtx_);
    // curFbo_不可用时，直接返回，不要继续申请内存
    if (!curFbo_) {
        return nullptr;
    }

    // 深拷贝当前FBO
    auto copyFbo = createFbo(curFbo_->size(), curFbo_->format());
    if (!copyFbo) {
        return nullptr;
    }

    copyFbo->bind();
    drawFbo(curFbo_, param);
    copyFbo->release();

    return copyFbo;
}

void DisplayRenderer::currentFrameToImage(const Stream::VideoProcessParam &param, const QSize &size, QImage &image)
{
    // 如果当前没有可用上下文，则退化到全局共享上下文在当前线程中所共享的上下文，如果退化失败，则返回
    QOpenGLContext *curContext = QOpenGLContext::currentContext();

    // 在此函数中被使用的上下文
    QOpenGLContext *usedContext = curContext;

    // 全局共享上下文在当前线程所对应的共享上下文
    QOpenGLContext *shareContext = nullptr;

    // 如果当前没有可用上下文，则查找renderer在此线程中的共享上下文
    if (!curContext) {
        // 未找到则返回
        shareContext = sharedContext();
        if (!shareContext)
            return;

        // 找到后，设为当前上下文
        shareContext->makeCurrent(shareContext->surface());
        usedContext = shareContext;
    }

    // 保存当前的viewport
    GLint prevViewport[4] = { 0, 0, 0, 0 };
    usedContext->functions()->glGetIntegerv(GL_VIEWPORT, prevViewport);

    // 渲染到帧缓冲，并保存到图片
    usedContext->functions()->glViewport(0, 0, size.width(), size.height());
    {
        auto frameBuffer = getFrameBuffer(param);
        if (frameBuffer) {
            image = frameBuffer->toImage();
        }
    }

    // 恢复现场
    usedContext->functions()->glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);

    // 如果切换过当前上下文，则进行恢复
    if (shareContext) {
        shareContext->doneCurrent();
    }
}

QSharedPointer<QOpenGLFramebufferObject> DisplayRenderer::acquireAvailableBackendBuffer(const QSize &size,
                                                                                        const QOpenGLFramebufferObjectFormat &fmt)
{
    if (!nextFbo_ || nextFbo_->size() != size) {
        nextFbo_ = createFbo(size, {});
    }

    return nextFbo_;
}

void DisplayRenderer::swap()
{
    QMutexLocker lock(&mtx_);
    std::swap(curFbo_, nextFbo_);
}

bool DisplayRenderer::initializeFboDrawResources()
{
    fboDrawProgram_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vsrc);
    fboDrawProgram_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fsrc);
    fboDrawProgram_.link();
    textureUniformLocation_ = fboDrawProgram_.uniformLocation("texture");
    zoomRectUniformLocation_ = fboDrawProgram_.uniformLocation("zoomRect");
    texFlipUniformLocation_ = fboDrawProgram_.uniformLocation("texFlip");
    brightnessUniformLocation_ = fboDrawProgram_.uniformLocation("brightness");
    contrastUniformLocation_ = fboDrawProgram_.uniformLocation("contrast");
    saturationUniformLocation_ = fboDrawProgram_.uniformLocation("saturation");
    hueUniformLocation_ = fboDrawProgram_.uniformLocation("hue");

    fboDrawProgram_.bind();
    fboDrawProgram_.setUniformValue(textureUniformLocation_, 0);
    fboDrawProgram_.release();

    // 全屏quad的顶点数据（交错式布局）
    const GLfloat vertices[] = { // 位置坐标               // 纹理坐标
                                 -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                 -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f
    };

    fboDrawVbo_.create();
    fboDrawVbo_.bind();
    fboDrawVbo_.allocate(vertices, sizeof(vertices));
    fboDrawVbo_.release();

    return true;
}

void DisplayRenderer::drawFbo(QSharedPointer<QOpenGLFramebufferObject> fbo, const Stream::VideoProcessParam &param)
{
    if (!fbo || !fboDrawResourcesInitialized_.load()) {
        return;
    }

    fboDrawProgram_.bind();
    fboDrawVbo_.bind();

    fboDrawProgram_.setUniformValue(zoomRectUniformLocation_, transToOpenGLUniform(param.digitalZoomRect));
    fboDrawProgram_.setUniformValue(texFlipUniformLocation_, getFlipParam(param.horizontalFlip, param.vecticalFlip));
    fboDrawProgram_.setUniformValue(brightnessUniformLocation_, param.brightness);
    fboDrawProgram_.setUniformValue(contrastUniformLocation_, param.contrast);
    fboDrawProgram_.setUniformValue(saturationUniformLocation_, param.saturation);
    fboDrawProgram_.setUniformValue(hueUniformLocation_, param.hue);

    // 设置顶点坐标并绘制
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

QSharedPointer<QOpenGLFramebufferObject> DisplayRenderer::createFbo(const QSize &size,
                                                                    const QOpenGLFramebufferObjectFormat &fmt)
{
    if (!size.isValid()) {
        return nullptr;
    }
    return QSharedPointer<QOpenGLFramebufferObject>::create(size, fmt);
}

QVector4D DisplayRenderer::transToOpenGLUniform(const QRectF &rect, bool needTrans) const
{
    return QVector4D(
        rect.x(),
        needTrans ? 1 - rect.y() - rect.height() : rect.y(),
        rect.width(),
        rect.height());
}

QVector2D DisplayRenderer::getFlipParam(bool hFlip, bool vFlip) const
{
    return QVector2D(
        hFlip ? 1.0f : 0.0f,
        vFlip ? 1.0f : 0.0f);
}

QOpenGLContext *DisplayRenderer::sharedContext()
{
    QOpenGLContext *shareContext = QOpenGLContext::globalShareContext();
    if (!shareContext)
        return nullptr;

    // 找到当前线程中和renderWorker的共享上下文，如果没有就返回
    const auto sharedContexts = shareContext->shareGroup()->shares();
    QOpenGLContext *curThreadRenderContext = nullptr;
    for (auto *const context : sharedContexts) {
        if (context->thread() == QThread::currentThread()) {
            curThreadRenderContext = context;
            break;
        }
    }

    return curThreadRenderContext;
}