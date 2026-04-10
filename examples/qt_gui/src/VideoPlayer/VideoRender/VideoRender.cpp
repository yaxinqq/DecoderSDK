#include "VideoRender.h"

#include <QDateTime>
#include <QOpenGLContext>
#include <QThread>

namespace utils {
#define EPSILON 1e-6
#define DOUBLEEPSILON 1e-12

bool equal(double a, double b, double epsilon = DOUBLEEPSILON)
{
    return std::fabs(a - b) < epsilon;
}

bool equal(float a, float b, double epsilon = EPSILON)
{
    return std::fabs(a - b) < epsilon;
}
} // namespace utils

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
        precision mediump float;
    #endif

        attribute vec4 position;
        attribute vec2 texCoord;
        varying vec2 vTexCoord;
        
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
const char *fsrc = R"(
    #ifdef GL_ES
        precision mediump float;
    #endif

        uniform sampler2D texture;
        uniform vec4 zoomRect;
        uniform vec2 texFlip;
        uniform float brightness;
        uniform float contrast;
        uniform float saturation;
        uniform float hue;
        varying vec2 vTexCoord;
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

VideoRender::VideoRender() : fboDrawResourcesInitialized_{false}, interopResourceReady_{false}
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
                             const Stream::VideoProcessParam &processParam)
{
    if (!frame || !frame->isValid()) {
        qWarning() << QStringLiteral("[VideoRender] Initialize failed: invalid input frame.");
        return;
    }

    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (!context) {
        qWarning() << QStringLiteral(
            "[VideoRender] Initialize failed: current OpenGL context is null.");
        return;
    }

    if (!tryBeginInitialize()) {
        const uint32_t logCount = skipInitializeLogCount_.fetch_add(1) + 1;
        if (logCount <= 2) {
            const auto initState = initState_.load();
            if (initState == InitState::kInitialized) {
                qInfo() << QStringLiteral("[VideoRender] Skip initialize: already initialized.");
            } else if (initState == InitState::kFailed) {
                qWarning() << QStringLiteral(
                    "[VideoRender] Skip initialize: initialization already failed before.");
            } else {
                qInfo() << QStringLiteral(
                    "[VideoRender] Skip initialize: initialization has already been attempted.");
            }
        }
        return;
    }

    auto failAndRollback = [&](const QString &reason) {
        qWarning() << QStringLiteral(
                          "[VideoRender] Initialize failed: %1, uninitialize can reset init state!")
                          .arg(reason);

        {
            QMutexLocker lock(&mtx_);
            curFbo_.reset();
            nextFbo_.reset();
        }

        fboDrawProgram_.removeAllShaders();
        if (fboDrawVbo_.isCreated()) {
            fboDrawVbo_.destroy();
        }
        fboDrawResourcesInitialized_.store(false);

        cleanupAllResources();

        interopResourceReady_.store(false);
        markInitializeFailed();
    };

    initializeOpenGLFunctions();

    // 如果配置文件中没有明确需要glFinish，则根据显卡型号，去开启，目前发现Intel总是需要开启的
    const QString vendor(reinterpret_cast<const char *>(glGetString(GL_VENDOR)));
    isIntelGpu_ = vendor.compare(QStringLiteral("Intel"), Qt::CaseInsensitive) == 0;

    // 设置视频处理参数
    processParam_ = processParam;

    // 初始化FBO
    const QSize fboSize(frame->width(), frame->height());
    if (!curFbo_ || curFbo_->size() != fboSize) {
        curFbo_ = createFbo(fboSize, {});
    }
    if (!nextFbo_ || nextFbo_->size() != fboSize) {
        nextFbo_ = createFbo(fboSize, {});
    }
    if (!curFbo_ || !nextFbo_) {
        failAndRollback(QStringLiteral("failed to create render FBO"));
        return;
    }

    // 初始化FBO绘制资源
    if (!fboDrawResourcesInitialized_.load()) {
        fboDrawResourcesInitialized_ =
            initializeFboDrawResources(QSize(frame->width(), frame->height()));
    }
    if (!fboDrawResourcesInitialized_.load()) {
        failAndRollback(QStringLiteral("failed to initialize FBO draw resources"));
        return;
    }

    // 根据视频处理参数，更新着色器
    if (!updateVideoProcessParams(processParam)) {
        failAndRollback(QStringLiteral("failed to update video process params"));
        return;
    }

    // 调用子类的初始化方法
    if (!initRenderVbo(false, false)) {
        failAndRollback(QStringLiteral("failed to initialize render VBO"));
        return;
    }

    if (!initRenderShader(*frame)) {
        failAndRollback(QStringLiteral("failed to initialize render shader"));
        return;
    }

    if (!initRenderTexture(*frame)) {
        failAndRollback(QStringLiteral("failed to initialize render texture"));
        return;
    }

    if (!initInteropsResource(*frame)) {
        failAndRollback(QStringLiteral("failed to initialize interop resources"));
        return;
    }

    interopResourceReady_.store(true);
    markInitializeSucceeded();

    // 查询是否支持glFence
    supportsGlFence_ = context->hasExtension(QByteArrayLiteral("GL_ARB_sync")) ||
                       context->hasExtension(QByteArrayLiteral("GL_OES_EGL_sync"));
    qInfo() << QStringLiteral("[VideoRender] Support glFence: %1")
                   .arg(supportsGlFence_ ? QStringLiteral("true") : QStringLiteral("false"));
    qInfo() << QStringLiteral("[VideoRender] Initialize success.");
}

void VideoRender::uninitialize()
{
    // 如果未初始化，则直接返回
    // 当前允许failed也能重置状态。后续如果希望收紧，可只让initialized状态通过
    if (initState_.load() == InitState::kNotAttempted)
        return;

    // 只保留curFbo和必要的渲染资源，其它全部释放
    {
        QMutexLocker lock(&mtx_);
        if (nextFbo_) {
            nextFbo_.reset();
        }
    }
    cleanupAllResources();

    // 重置初始化相关的标志
    interopResourceReady_.store(false);
    resetInitializeState();
}

void VideoRender::render(const std::shared_ptr<decoder_sdk::Frame> &frame,
                         Stream::VideoFrameParam *videoFrameParam)
{
    if (!frame || !frame->isValid() || !isValid()) {
        return;
    }

    // 处理裁剪拼接
    QSize stitchOutputSize;
    // 判断是否启用了裁剪拼接
    const bool stitchEnabled = [&]() {
        QMutexLocker lock(&clippingMtx_);
        stitchOutputSize = stitchOutputSize_;
        return !clippingSourceRegions_.isEmpty() &&
               clippingSourceRegions_.size() == stitchTargetRegions_.size() &&
               stitchOutputSize_.isValid();
    }();
    // 得到最终输出纹理的大小，如果未启用裁剪拼接，则使用纹理的原始大小
    const QSize finalOutputSize =
        stitchEnabled ? stitchOutputSize : QSize(frame->width(), frame->height());

    // 渲染目标
    QSharedPointer<QOpenGLFramebufferObject> renderTargetFbo;
    // 最终输出
    QSharedPointer<QOpenGLFramebufferObject> outputFbo;
    {
        // 如果双缓冲中的下一帧纹理大小和最终输出纹理大小不一致，则重新创建
        QMutexLocker lock(&mtx_);
        if (!nextFbo_ || nextFbo_->size() != finalOutputSize) {
            nextFbo_ = createFbo(finalOutputSize, {});
        }
        if (!nextFbo_) {
            return;
        }
        outputFbo = nextFbo_;
    }

    if (stitchEnabled) {
        // 如果开启了裁剪拼接，则根据纹理的原始大小，创建渲染目标缓冲区
        QMutexLocker lock(&clippingMtx_);
        if (!clippingInputFbo_ ||
            clippingInputFbo_->size() != QSize(frame->width(), frame->height())) {
            clippingInputFbo_ = createFbo(QSize(frame->width(), frame->height()), {});
        }
        renderTargetFbo = clippingInputFbo_;
    } else {
        // 如果未开启裁剪拼接，则渲染目标和最终输出一致
        renderTargetFbo = outputFbo;
    }

    // 检查异常值
    if (!renderTargetFbo || !outputFbo) {
        return;
    }

    // 绑定渲染目标FBO并让子类渲染到其中
    renderTargetFbo->bind();
    glViewport(0, 0, frame->width(), frame->height());
    // clearGL();
    const bool success = renderFrame(*frame);
    renderTargetFbo->release();
    // 如果渲染成功，且开启了拼接，则将渲染目标纹理的区域，按格式拷贝到最终输出
    if (success && stitchEnabled) {
        applyClippingStitch(renderTargetFbo, outputFbo);
    }

    // GPU同步
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

    if (success) {
        QMutexLocker lock(&mtx_);
        std::swap(curFbo_, nextFbo_);

        // 更新参数
        if (videoFrameParam) {
            videoFrameParam->size = curFbo_->size();
            videoFrameParam->pts = frame->secPts();
        }
    }
}

void VideoRender::draw()
{
    if (!fboDrawResourcesInitialized_.load()) {
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

    copyFbo->bind();
    drawFbo(curFbo_);
    copyFbo->release();

    return copyFbo;
}

bool VideoRender::isValid() const
{
    return interopResourceReady_.load() && fboDrawResourcesInitialized_.load();
}

bool VideoRender::isInitialized() const
{
    return initState_.load() == InitState::kInitialized;
}

bool VideoRender::shouldRebuild() const
{
    return false;
}

void VideoRender::setDigitalZoomRect(const QRectF &rect)
{
    if (setParamIfChanged(processParam_.digitalZoomRect, rect,
                          [](const QRectF &lhs, const QRectF &rhs) { return lhs == rhs; })) {
        markProcessParamDirty(kDirtyZoomRect);
    }
}

QRectF VideoRender::digitalZoomRect() const
{
    return processParam_.digitalZoomRect;
}

void VideoRender::setHorizontalFlip(bool flip)
{
    if (setParamIfChanged(processParam_.horizontalFlip, flip,
                          [](bool lhs, bool rhs) { return lhs == rhs; })) {
        markProcessParamDirty(kDirtyFlip);
    }
}

bool VideoRender::isHorizontalFlip() const
{
    return processParam_.horizontalFlip;
}

void VideoRender::setVecticalFlip(bool flip)
{
    if (setParamIfChanged(processParam_.vecticalFlip, flip,
                          [](bool lhs, bool rhs) { return lhs == rhs; })) {
        markProcessParamDirty(kDirtyFlip);
    }
}

bool VideoRender::isVecticalFlip() const
{
    return processParam_.vecticalFlip;
}

void VideoRender::setHorizontalAndVecticalFlip(bool hflip, bool vflip)
{
    const bool horizontalChanged = setParamIfChanged(processParam_.horizontalFlip, hflip,
                                                     [](bool lhs, bool rhs) { return lhs == rhs; });
    const bool vecticalChanged = setParamIfChanged(processParam_.vecticalFlip, vflip,
                                                   [](bool lhs, bool rhs) { return lhs == rhs; });
    if (horizontalChanged || vecticalChanged) {
        markProcessParamDirty(kDirtyFlip);
    }
}

QPair<bool, bool> VideoRender::isHorizontalAndVecticalFlip() const
{
    return {processParam_.horizontalFlip, processParam_.vecticalFlip};
}

void VideoRender::setColorAdjustments(float brightness, float contrast, float saturation, float hue)
{
    brightness = clampBrightness(brightness);
    contrast = clampContrast(contrast);
    saturation = clampSaturation(saturation);
    hue = clampHue(hue);

    uint32_t flags = kDirtyNone;
    if (setParamIfChanged(processParam_.brightness, brightness,
                          [](float lhs, float rhs) { return utils::equal(lhs, rhs); })) {
        flags |= kDirtyBrightness;
    }
    if (setParamIfChanged(processParam_.contrast, contrast,
                          [](float lhs, float rhs) { return utils::equal(lhs, rhs); })) {
        flags |= kDirtyContrast;
    }
    if (setParamIfChanged(processParam_.saturation, saturation,
                          [](float lhs, float rhs) { return utils::equal(lhs, rhs); })) {
        flags |= kDirtySaturation;
    }
    if (setParamIfChanged(processParam_.hue, hue,
                          [](float lhs, float rhs) { return utils::equal(lhs, rhs); })) {
        flags |= kDirtyHue;
    }

    if (flags != kDirtyNone) {
        markProcessParamDirty(flags);
    }
}

void VideoRender::setBrightness(float brightness)
{
    brightness = clampBrightness(brightness);
    if (setParamIfChanged(processParam_.brightness, brightness,
                          [](float lhs, float rhs) { return utils::equal(lhs, rhs); })) {
        markProcessParamDirty(kDirtyBrightness);
    }
}

float VideoRender::brightness() const
{
    return processParam_.brightness;
}

void VideoRender::setContrast(float contrast)
{
    contrast = clampContrast(contrast);
    if (setParamIfChanged(processParam_.contrast, contrast,
                          [](float lhs, float rhs) { return utils::equal(lhs, rhs); })) {
        markProcessParamDirty(kDirtyContrast);
    }
}

float VideoRender::contrast() const
{
    return processParam_.contrast;
}

void VideoRender::setSaturation(float saturation)
{
    saturation = clampSaturation(saturation);
    if (setParamIfChanged(processParam_.saturation, saturation,
                          [](float lhs, float rhs) { return utils::equal(lhs, rhs); })) {
        markProcessParamDirty(kDirtySaturation);
    }
}

float VideoRender::saturation() const
{
    return processParam_.saturation;
}

void VideoRender::setHue(float hue)
{
    hue = clampHue(hue);
    if (setParamIfChanged(processParam_.hue, hue,
                          [](float lhs, float rhs) { return utils::equal(lhs, rhs); })) {
        markProcessParamDirty(kDirtyHue);
    }
}

float VideoRender::hue() const
{
    return processParam_.hue;
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

bool VideoRender::updateVideoProcessParams(const Stream::VideoProcessParam &processParam)
{
    const auto ret = fboDrawProgram_.bind();
    if (!ret)
        return false;

    fboDrawProgram_.setUniformValue("texture", 0);
    fboDrawProgram_.setUniformValue("zoomRect", transToOpenGLUniform(processParam.digitalZoomRect));
    fboDrawProgram_.setUniformValue(
        "texFlip", getFlipParam(processParam.horizontalFlip, processParam.vecticalFlip));
    fboDrawProgram_.setUniformValue("brightness", processParam.brightness);
    fboDrawProgram_.setUniformValue("contrast", processParam.contrast);
    fboDrawProgram_.setUniformValue("saturation", processParam.saturation);
    fboDrawProgram_.setUniformValue("hue", processParam.hue);

    fboDrawProgram_.release();
    processParamDirtyFlags_.store(kDirtyNone);
    return true;
}

void VideoRender::drawFbo(QSharedPointer<QOpenGLFramebufferObject> fbo)
{
    if (!fbo || !fboDrawResourcesInitialized_.load()) {
        return;
    }

    fboDrawProgram_.bind();
    fboDrawVbo_.bind();

    const uint32_t dirtyFlags = consumeProcessParamDirtyFlags();
    if ((dirtyFlags & kDirtyZoomRect) != 0) {
        fboDrawProgram_.setUniformValue("zoomRect",
                                        transToOpenGLUniform(processParam_.digitalZoomRect));
    }
    if ((dirtyFlags & kDirtyFlip) != 0) {
        fboDrawProgram_.setUniformValue(
            "texFlip", getFlipParam(processParam_.horizontalFlip, processParam_.vecticalFlip));
    }
    if ((dirtyFlags & kDirtyBrightness) != 0) {
        fboDrawProgram_.setUniformValue("brightness", processParam_.brightness);
    }
    if ((dirtyFlags & kDirtyContrast) != 0) {
        fboDrawProgram_.setUniformValue("contrast", processParam_.contrast);
    }
    if ((dirtyFlags & kDirtySaturation) != 0) {
        fboDrawProgram_.setUniformValue("saturation", processParam_.saturation);
    }
    if ((dirtyFlags & kDirtyHue) != 0) {
        fboDrawProgram_.setUniformValue("hue", processParam_.hue);
    }

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

void VideoRender::markProcessParamDirty(uint32_t flags)
{
    processParamDirtyFlags_.fetch_or(flags, std::memory_order_relaxed);
}

uint32_t VideoRender::consumeProcessParamDirtyFlags()
{
    return processParamDirtyFlags_.exchange(kDirtyNone, std::memory_order_relaxed);
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

QVector4D VideoRender::transToOpenGLUniform(const QRectF &rect, bool needTrans) const
{
    return QVector4D(rect.x(), needTrans ? 1 - rect.y() - rect.height() : rect.y(), rect.width(),
                     rect.height());
}

QVector2D VideoRender::getFlipParam(bool hFlip, bool vFlip) const
{
    return QVector2D(hFlip ? 1.0f : 0.0f, vFlip ? 1.0f : 0.0f);
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
    GLint prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    // 开始裁剪拼接
    // 设置读、写纹理，并设置默认北京
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
