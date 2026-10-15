#include "VideoRender.h"
#include "DisplayRenderer.h"
#include "../Base/InternalUtils.h"

#include <QDateTime>
#include <QOpenGLContext>
#include <QThread>

namespace {
    // 钳制亮度
    float clampBrightness(const float value)
    {
        return std::clamp(value, -1.0f, 1.0f);
    }
    // 钳制对比度
    float clampContrast(const float value)
    {
        return std::clamp(value, 0.0f, 3.0f);
    }
    // 钳制饱和度
    float clampSaturation(const float value)
    {
        return std::clamp(value, 0.0f, 2.0f);
    }
    // 钳制色调旋转角度
    float clampHue(const float value)
    {
        return std::clamp(value, -1.0f, 1.0f);
    }

    // 比较参数是否改变，如果改变则更新
    template <typename T, typename Equal>
    bool setParamIfChanged(T &target, const T &value, Equal equal)
    {
        if (equal(target, value)) {
            return false;
        }
        target = value;
        return true;
    }

    // 统计有效DisplayRenderer数量，避免为多target重复shader渲染
    bool hasMultipleValidRenderers(
        const std::vector<std::shared_ptr<DisplayRenderer>> &renderers)
    {
        return renderers.size() > 1;
    }

    // 帧拷贝区域（源+目标）
    struct BlitRect {
        int srcX0 = 0;
        int srcY0 = 0;
        int srcX1 = 0;
        int srcY1 = 0;
        int dstX0 = 0;
        int dstY0 = 0;
        int dstX1 = 0;
        int dstY1 = 0;
    };

    // 将传入的QRectF转为自定义的帧拷贝区域，并且转为OpenGL坐标系（y从下到上）
    bool convertToBlitRect(const QRectF &src, const QRectF &dst, int inputHeight, int outputHeight,
                           BlitRect &blitRect)
    {
        if (!src.isValid() || !dst.isValid()) {
            return false;
        }

        // 处理源区域，并确保不溢出
        blitRect.srcX0 = static_cast<int>(std::lround(src.left()));
        blitRect.srcX1 = static_cast<int>(std::lround(src.right()));
        blitRect.srcY0 = inputHeight - static_cast<int>(std::lround(src.bottom()));
        blitRect.srcY1 = inputHeight - static_cast<int>(std::lround(src.top()));

        // 处理目标区域，并确保不溢出
        blitRect.dstX0 = static_cast<int>(std::lround(dst.left()));
        blitRect.dstX1 = static_cast<int>(std::lround(dst.right()));
        blitRect.dstY0 = outputHeight - static_cast<int>(std::lround(dst.bottom()));
        blitRect.dstY1 = outputHeight - static_cast<int>(std::lround(dst.top()));

        return blitRect.srcX0 != blitRect.srcX1 && blitRect.srcY0 != blitRect.srcY1 &&
               blitRect.dstX0 != blitRect.dstX1 && blitRect.dstY0 != blitRect.dstY1;
    }

    // 处理直接转换的区域
    bool prepareDirectScaleRects(const QRectF &sourceRect, const QRectF &targetRect,
                                 const QRectF &inputBounds, const QRectF &outputBounds, QRectF &outSrc,
                                 QRectF &outDst)
    {
        // 保持原样输出即可，OpenGL在绘制时，会根据纹理的设置进行自动缩放
        Q_UNUSED(inputBounds);
        Q_UNUSED(outputBounds);
        outSrc = sourceRect;
        outDst = targetRect;
        return outSrc.isValid() && outDst.isValid();
    }

    // 源贴图的一块区域，按原尺寸贴到目标位置，如果越界就双向裁剪，并保持像素一一对应关系
    bool prepareKeepSourceSizeRects(const QRectF &sourceRect, const QRectF &targetRect,
                                    const QRectF &inputBounds, const QRectF &outputBounds,
                                    QRectF &outSrc, QRectF &outDst)
    {
        // 裁剪区域需要在输入的有效区域内
        const QRectF clippedSource = sourceRect.intersected(inputBounds);
        if (!clippedSource.isValid() || !targetRect.isValid()) {
            return false;
        }

        // 锚定矩形，以左上点为准，source尺寸贴到target位置
        const QRectF anchorRect(targetRect.left(), targetRect.top(), clippedSource.width(),
                                clippedSource.height());
        // 目标区域
        const QRectF targetBounds(targetRect.left(), targetRect.top(), targetRect.width(),
                                  targetRect.height());
        // 裁剪目标，锚定区域找和目标区域交集，同时和输出区域求交集。确保区域不溢出
        const QRectF clippedTarget = anchorRect.intersected(targetBounds).intersected(outputBounds);
        if (!clippedTarget.isValid()) {
            return false;
        }

        // 计算偏移，目标被裁剪后，相对于锚定区域偏了多少
        const double offsetX = clippedTarget.left() - anchorRect.left();
        const double offsetY = clippedTarget.top() - anchorRect.top();
        // 反推source区域
        outSrc = QRectF(clippedSource.left() + offsetX, clippedSource.top() + offsetY,
                        clippedTarget.width(), clippedTarget.height());
        // 输出区域
        outDst = clippedTarget;
        return outSrc.isValid() && outDst.isValid();
    }

    // 根据裁剪策略准备矩形区域
    bool prepareStrategyRects(VideoRender::ClippingStitchStrategy strategy, const QRectF &sourceRect,
                              const QRectF &targetRect, const QRectF &inputBounds,
                              const QRectF &outputBounds, QRectF &outSrc, QRectF &outDst)
    {
        if (strategy == VideoRender::ClippingStitchStrategy::kKeepSourceSizeTopLeft) {
            return prepareKeepSourceSizeRects(sourceRect, targetRect, inputBounds, outputBounds, outSrc,
                                              outDst);
        }
        return prepareDirectScaleRects(sourceRect, targetRect, inputBounds, outputBounds, outSrc,
                                       outDst);
    }

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

VideoRender::VideoRender()
    : interopResourceReady_{ false }
{
    forceGpuFinish_ = false;
}

VideoRender::~VideoRender()
{
}

void VideoRender::initialize(const decoder_sdk::Frame &frame)
{
    if (!frame.isValid()) {
        qWarning() << QStringLiteral("[VideoRender] Initialize failed: invalid input frame.");
        return;
    }

    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (!context) {
        qWarning() << QStringLiteral("[VideoRender] Initialize failed: current OpenGL context is null.");
        return;
    }

    if (!tryBeginInitialize()) {
        const uint32_t logCount = skipInitializeLogCount_.fetch_add(1) + 1;
        if (logCount <= 2) {
            const auto initState = initState_.load();
            if (initState == InitState::kInitialized) {
                qInfo() << QStringLiteral("[VideoRender] Skip initialize: already initialized.");
            } else if (initState == InitState::kFailed) {
                qWarning() << QStringLiteral("[VideoRender] Skip initialize: initialization already failed before.");
            } else {
                qInfo() << QStringLiteral("[VideoRender] Skip initialize: initialization has already been attempted.");
            }
        }
        return;
    }

    auto failAndRollback = [&](const QString &reason) {
        qWarning() << QStringLiteral("[VideoRender] Initialize failed: %1, uninitialize can reset init state!").arg(reason);
        cleanupAllResources();

        interopResourceReady_.store(false);
        markInitializeFailed();
    };

    initializeOpenGLFunctions();

    // 如果配置文件中没有明确需要glFinish，则根据显卡型号，去开启，目前发现Intel总是需要开启的
    const QString vendor(reinterpret_cast<const char *>(glGetString(GL_VENDOR)));
    isIntelGpu_ = vendor.compare(QStringLiteral("Intel"), Qt::CaseInsensitive) == 0;

    // 调用子类的初始化方法
    if (!initRenderVbo(false, false)) {
        failAndRollback(QStringLiteral("failed to initialize render VBO"));
        return;
    }

    if (!initRenderShader(frame)) {
        failAndRollback(QStringLiteral("failed to initialize render shader"));
        return;
    }

    if (!initRenderTexture(frame)) {
        failAndRollback(QStringLiteral("failed to initialize render texture"));
        return;
    }

    if (!initInteropsResource(frame)) {
        failAndRollback(QStringLiteral("failed to initialize interop resources"));
        return;
    }

    interopResourceReady_.store(true);
    markInitializeSucceeded();

    // 查询是否支持glFence
    supportsGlFence_ = context->hasExtension(QByteArrayLiteral("GL_ARB_sync")) ||
                       context->hasExtension(QByteArrayLiteral("GL_OES_EGL_sync"));
    qInfo() << QStringLiteral("[VideoRender] Support glFence: %1").arg(supportsGlFence_ ? QStringLiteral("true") : QStringLiteral("false"));
    qInfo() << QStringLiteral("[VideoRender] Initialize success.");
}

void VideoRender::uninitialize()
{
    // 如果未初始化，则直接返回
    // 当前允许failed也能重置状态。后续如果希望收紧，可只让initialized状态通过
    if (initState_.load() == InitState::kNotAttempted)
        return;

    // 只保留必要的渲染资源，其它全部释放
    cleanupAllResources();
    commonRenderTargetFbo_.reset();
    clippingInputFbo_.reset();

    // 重置初始化相关的标志
    interopResourceReady_.store(false);
    resetInitializeState();
}

void VideoRender::render(const decoder_sdk::Frame &frame,
                         const std::vector<std::shared_ptr<DisplayRenderer>> &renderers,
                         Stream::VideoFrameParam *videoFrameParam)
{
    // 渲染入口：每帧只应做一次解码器帧到OpenGL纹理的interop/上传，
    // 后续所有DisplayRenderer共享这份纹理结果，只分别写自己的后台FBO。
    if (!frame.isValid() || !isValid()) {
        // 帧无效或interop资源未就绪时不能进入GL绘制，提前返回避免子类访问无效纹理。
        return;
    }

    QSize stitchOutputSize;
    // 裁剪拼接状态可能由外部线程更新，这里统一锁读取一次，
    // 保证本帧后面的判断和FBO大小都基于同一份状态快照。
    const bool stitchEnabled = readClippingStitchState(stitchOutputSize);
    // 拼接模式下最终输出是多个源区域拼出的画布，普通模式则是帧本身尺寸；
    // 后续FBO申请、viewport和blit都必须以该尺寸为准，不能继续使用frame原始尺寸。
    const QSize finalOutputSize =
        stitchEnabled ? stitchOutputSize : QSize(frame.width(), frame.height());

    // 拼接模式需要先把整帧渲染到公共输入FBO，再按区域裁剪到各展示目标。
    // 提前补齐/重建该FBO，避免interop完成后再发现没有可绑定的中间纹理。
    prepareClippingInputFboIfNeeded(stitchEnabled, frame);

    // 软件解码和硬件解码在这里分别把CPU帧或显存帧转成当前OpenGL上下文可采样的纹理；
    // 这一步失败后不能继续渲染，否则后续draw/blit会读取到未更新或无效纹理。
    bool success = interopToOpenGL(frame);
    QSharedPointer<QOpenGLFramebufferObject> commonSourceFbo;
    if (stitchEnabled) {
        // 裁剪拼接只渲染一次到公共输入FBO，之后每个DisplayRenderer只做区域blit。
        success = renderClippingSource(frame, success);
    } else {
        // 非拼接且存在多个有效DisplayRenderer时，先渲染一次公共中间FBO，
        // 避免每个display target都重复执行shader；单target或FBO申请失败时，
        // commonSourceFbo为空，分发阶段会自动退回直接渲染到目标FBO。
        commonSourceFbo = acquireCommonRenderTarget(
            finalOutputSize, hasMultipleValidRenderers(renderers));
        if (success && commonSourceFbo) {
            success = renderFrameToFbo(commonSourceFbo, finalOutputSize, frame);
        }
    }

    if (success) {
        // 源纹理已经就绪后才开始分发；各DisplayRenderer只获取后台FBO并执行
        // 整帧blit、区域裁剪blit，或FBO创建失败时的直接draw，不再重复interop。
        success = dispatchToRenderers(frame, renderers, finalOutputSize, stitchEnabled,
                                      commonSourceFbo);
    }

    // 当前实现会等待GPU完成本帧，确保下面的cleanupRenderResources()不会释放仍在
    // GPU命令流中的资源。这里是明确的同步点，后续改成多帧复用+延迟fence时可替换。
    synchronizeGpu();
    // 子类在GPU同步完成后再释放本轮专用纹理/fence/资源，避免提前销毁导致GPU访问悬挂资源。
    cleanupRenderResources();

    if (success) {
        // 只有整帧都成功分发后才把后台FBO交换到前台，防止上层显示半渲染或未更新的target。
        presentToRenderers(renderers);
        // 参数回写放在swap之后，保证纹理Ready事件拿到的尺寸/PTS与最终前台FBO属于同一帧。
        updateVideoFrameParam(videoFrameParam, finalOutputSize, frame);
    }
}

bool VideoRender::isValid() const
{
    return interopResourceReady_.load();
}

bool VideoRender::isInitialized() const
{
    return initState_.load() == InitState::kInitialized;
}

bool VideoRender::shouldRebuild() const
{
    return false;
}

void VideoRender::setClippingStitchRegions(const QVector<QRect> &sourceRegions,
                                           const QVector<QRect> &targetRegions)
{
    QMutexLocker lock(&clippingMtx_);
    // 和当前区域进行比较，看是否一致，一致则不处理
    if (sourceRegions == clippingSourceRegions_ && targetRegions == stitchTargetRegions_) {
        return;
    }

    clippingInputFbo_.reset();
    clippingSourceRegions_.clear();
    stitchTargetRegions_.clear();
    stitchOutputSize_ = QSize();

    // 根据双方的最小值进行处理
    const int pairCount = std::min(sourceRegions.size(), targetRegions.size());
    clippingSourceRegions_.reserve(pairCount);
    stitchTargetRegions_.reserve(pairCount);

    // 保存多区域的AABB
    int maxRight = 0;
    int maxBottom = 0;
    for (int i = 0; i < pairCount; ++i) {
        // 判断区域是否合法
        const QRect sourceRect = sourceRegions.at(i).normalized();
        const QRect targetRect = targetRegions.at(i).normalized();
        if (!sourceRect.isValid() || !targetRect.isValid() || targetRect.x() < 0 ||
            targetRect.y() < 0) {
            continue;
        }

        // 保存结果，并更新区域大小
        clippingSourceRegions_.push_back(sourceRect);
        stitchTargetRegions_.push_back(targetRect);
        maxRight = std::max(maxRight, targetRect.x() + targetRect.width());
        maxBottom = std::max(maxBottom, targetRect.y() + targetRect.height());
    }

    // 检查异常值
    if (clippingSourceRegions_.isEmpty() || maxRight <= 0 || maxBottom <= 0) {
        clippingSourceRegions_.clear();
        stitchTargetRegions_.clear();
        return;
    }

    // 最终输出大小
    stitchOutputSize_ = QSize(maxRight, maxBottom);
}

void VideoRender::clearClippingStitchRegions()
{
    QMutexLocker lock(&clippingMtx_);
    clippingInputFbo_.reset();
    clippingSourceRegions_.clear();
    stitchTargetRegions_.clear();
    stitchOutputSize_ = QSize();
}

bool VideoRender::hasClippingStitchRegions() const
{
    QMutexLocker lock(&clippingMtx_);
    return !clippingSourceRegions_.isEmpty() &&
           clippingSourceRegions_.size() == stitchTargetRegions_.size() &&
           stitchOutputSize_.isValid();
}

void VideoRender::setClippingStitchStrategy(ClippingStitchStrategy strategy)
{
    QMutexLocker lock(&clippingMtx_);
    clippingStitchStrategy_ = strategy;
}

VideoRender::ClippingStitchStrategy VideoRender::clippingStitchStrategy() const
{
    QMutexLocker lock(&clippingMtx_);
    return clippingStitchStrategy_;
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

bool VideoRender::tryBeginInitialize()
{
    auto expected = InitState::kNotAttempted;
    return initState_.compare_exchange_strong(expected, InitState::kInitializing);
}

void VideoRender::markInitializeFailed()
{
    initState_.store(InitState::kFailed);
}

void VideoRender::markInitializeSucceeded()
{
    initState_.store(InitState::kInitialized);
}

void VideoRender::resetInitializeState()
{
    initState_.store(InitState::kNotAttempted);
    skipInitializeLogCount_.store(0);
}

QSharedPointer<QOpenGLFramebufferObject> VideoRender::createFbo(
    const QSize &size, const QOpenGLFramebufferObjectFormat &fmt)
{
    if (!size.isValid()) {
        return nullptr;
    }
    return QSharedPointer<QOpenGLFramebufferObject>::create(size, fmt);
}

bool VideoRender::readClippingStitchState(QSize &outputSize) const
{
    QMutexLocker lock(&clippingMtx_);
    outputSize = stitchOutputSize_;
    return !clippingSourceRegions_.isEmpty() &&
           clippingSourceRegions_.size() == stitchTargetRegions_.size() &&
           stitchOutputSize_.isValid();
}

void VideoRender::prepareClippingInputFboIfNeeded(bool stitchEnabled,
                                                  const decoder_sdk::Frame &frame)
{
    if (!stitchEnabled) {
        return;
    }

    QMutexLocker lock(&clippingMtx_);
    const QSize inputSize(frame.width(), frame.height());
    if (!clippingInputFbo_ || clippingInputFbo_->size() != inputSize) {
        clippingInputFbo_ = createFbo(inputSize, {});
    }
}

bool VideoRender::renderFrameToFbo(const QSharedPointer<QOpenGLFramebufferObject> &targetFbo,
                                   const QSize &viewportSize,
                                   const decoder_sdk::Frame &frame)
{
    if (!targetFbo) {
        return false;
    }

    if (!targetFbo->bind()) {
        qWarning() << QStringLiteral("[VideoRender] Failed to bind render target fbo!");
        return false;
    }
    glViewport(0, 0, viewportSize.width(), viewportSize.height());
    const bool renderSucceeded = renderToFbo(frame);
    targetFbo->release();
    return renderSucceeded;
}

bool VideoRender::renderClippingSource(const decoder_sdk::Frame &frame,
                                       bool interopSucceeded)
{
    if (!interopSucceeded || !clippingInputFbo_) {
        return false;
    }

    return renderFrameToFbo(clippingInputFbo_, QSize(frame.width(), frame.height()), frame);
}

QSharedPointer<QOpenGLFramebufferObject> VideoRender::acquireCommonRenderTarget(
    const QSize &finalOutputSize, bool useCommonRenderTarget)
{
    if (!useCommonRenderTarget) {
        return nullptr;
    }

    QMutexLocker lock(&clippingMtx_);
    if (!commonRenderTargetFbo_ || commonRenderTargetFbo_->size() != finalOutputSize) {
        commonRenderTargetFbo_ = createFbo(finalOutputSize, {});
    }
    return commonRenderTargetFbo_;
}

bool VideoRender::dispatchToRenderers(
    const decoder_sdk::Frame &frame,
    const std::vector<std::shared_ptr<DisplayRenderer>> &renderers,
    const QSize &finalOutputSize, bool stitchEnabled,
    const QSharedPointer<QOpenGLFramebufferObject> &commonSourceFbo)
{
    bool allSucceeded = true;
    for (const auto &renderer : renderers) {
        if (!renderer) {
            continue;
        }

        const auto outputFbo = renderer->acquireAvailableBackendBuffer(finalOutputSize, {});
        if (!outputFbo) {
            qWarning() << QStringLiteral(
                "[VideoRender] Can not get display renderer backend fbo!");
            continue;
        }

        bool targetSucceeded = false;
        if (stitchEnabled) {
            if (!clippingInputFbo_) {
                qWarning() << QStringLiteral("[VideoRender] Clipping input fbo is null!");
            } else {
                targetSucceeded = applyClippingStitch(clippingInputFbo_, outputFbo);
            }
        } else if (commonSourceFbo) {
            targetSucceeded = blitFbo(commonSourceFbo, outputFbo);
        } else {
            targetSucceeded = renderFrameToFbo(outputFbo, finalOutputSize, frame);
        }

        if (!targetSucceeded) {
            allSucceeded = false;
        }
    }
    return allSucceeded;
}

void VideoRender::synchronizeGpu()
{
    if (forceGpuFinish_ || isIntelGpu_) {
        glFinish();
        return;
    }

    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    const auto waitResult =
        glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
    if (waitResult == GL_WAIT_FAILED || waitResult == GL_TIMEOUT_EXPIRED) {
        qWarning() << QStringLiteral("[VideoRender] glClientWaitSync failed!");
    }
    glDeleteSync(fence);
}

void VideoRender::presentToRenderers(
    const std::vector<std::shared_ptr<DisplayRenderer>> &renderers)
{
    for (const auto &renderer : renderers) {
        if (renderer) {
            renderer->swap();
        }
    }
}

void VideoRender::updateVideoFrameParam(Stream::VideoFrameParam *videoFrameParam,
                                        const QSize &finalOutputSize,
                                        const decoder_sdk::Frame &frame) const
{
    if (!videoFrameParam) {
        return;
    }

    videoFrameParam->size = finalOutputSize;
    videoFrameParam->pts = frame.secPts();
}

bool VideoRender::blitFbo(const QSharedPointer<QOpenGLFramebufferObject> &sourceFbo,
                          const QSharedPointer<QOpenGLFramebufferObject> &outputFbo)
{
    if (!sourceFbo || !outputFbo || sourceFbo->size() != outputFbo->size()) {
        return false;
    }

    GLint prevReadFbo = 0;
    GLint prevDrawFbo = 0;
    GLint prevViewport[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFbo->handle());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, outputFbo->handle());
    glViewport(0, 0, outputFbo->width(), outputFbo->height());
    glBlitFramebuffer(0, 0, outputFbo->width(), outputFbo->height(),
                      0, 0, outputFbo->width(), outputFbo->height(),
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    return true;
}

bool VideoRender::applyClippingStitch(const QSharedPointer<QOpenGLFramebufferObject> &sourceFbo,
                                      const QSharedPointer<QOpenGLFramebufferObject> &outputFbo)
{
    // 得到源区域和目标区域
    QVector<QRect> sourceRegions;
    QVector<QRect> targetRegions;
    ClippingStitchStrategy strategy = ClippingStitchStrategy::kScaleToTarget;
    {
        QMutexLocker lock(&clippingMtx_);
        sourceRegions = clippingSourceRegions_;
        targetRegions = stitchTargetRegions_;
        strategy = clippingStitchStrategy_;
    }

    // 不合法的区域或入参直接返回
    if (!sourceFbo || !outputFbo || sourceRegions.isEmpty() || targetRegions.isEmpty()) {
        return false;
    }

    // 以两区域中最小的个数进行处理
    const int pairCount = std::min(sourceRegions.size(), targetRegions.size());
    if (pairCount <= 0) {
        return false;
    }

    // 判断纹理尺寸是否合规
    const QSize inputSize = sourceFbo->size();
    const QSize outputSize = outputFbo->size();
    if (!inputSize.isValid() || !outputSize.isValid()) {
        return false;
    }

    // 输入区域、输出区域
    const QRectF inputBounds(0.0, 0.0, static_cast<double>(inputSize.width()),
                             static_cast<double>(inputSize.height()));
    const QRectF outputBounds(0.0, 0.0, static_cast<double>(outputSize.width()),
                              static_cast<double>(outputSize.height()));

    // 保存现场
    GLint prevReadFbo = 0;
    GLint prevDrawFbo = 0;
    GLint prevViewport[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    // 开始裁剪拼接
    // 设置读、写纹理，并设置默认背景
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFbo->handle());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, outputFbo->handle());
    glViewport(0, 0, outputSize.width(), outputSize.height());
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // 按照区域进行处理
    const int inputHeight = inputSize.height();
    const int outputHeight = outputSize.height();
    for (int i = 0; i < pairCount; ++i) {
        // 准备源区域、目标区域
        QRectF src;
        QRectF dst;
        if (!prepareStrategyRects(strategy, QRectF(sourceRegions.at(i)),
                                  QRectF(targetRegions.at(i)), inputBounds, outputBounds, src,
                                  dst)) {
            continue;
        }

        // 准备帧拷贝区域
        BlitRect blitRect;
        if (!convertToBlitRect(src, dst, inputHeight, outputHeight, blitRect)) {
            continue;
        }

        // 拷贝
        glBlitFramebuffer(blitRect.srcX0, blitRect.srcY0, blitRect.srcX1, blitRect.srcY1,
                          blitRect.dstX0, blitRect.dstY0, blitRect.dstX1, blitRect.dstY1,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }

    // 恢复现场
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);

    return true;
}
