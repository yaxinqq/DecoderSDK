#include "RenderBufferQueue.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QOpenGLContext>
#include <QScreen>

#include <algorithm>

#define RENDER_BUFFER_QUEUE_DEBUG 0

namespace {
// 流畅度优化参数
constexpr int kMinFenceCheckInterval = 1;  // 最小fence检查间隔(ms)
constexpr int kMaxFenceCheckInterval = 8;  // 最大fence检查间隔(ms)
constexpr int kForceCleanupInterval = 100; // 强制清理间隔(ms)
constexpr int kFenceMicroWaitNs = 1000;    // fence微等待时间(纳秒)
} // namespace

RenderBufferQueue::RenderBufferQueue(int bufferCount)
{
    buffers_.reserve(bufferCount);
    for (int i = 0; i < bufferCount; ++i) {
        buffers_.emplace_back(std::make_unique<RenderBuffer>());
    }

    globalTimer_.start();
    lastFenceCheckTime_ = 0;
}

RenderBufferQueue::~RenderBufferQueue()
{
    cleanup();
}

bool RenderBufferQueue::initialize(const QSize &size, const QOpenGLFramebufferObjectFormat &format,
                                   double avgFrameInterval)
{
    QMutexLocker locker(&mutex_);

    if (initialized_ || !size.isValid()) {
        return false;
    }

    initializeOpenGLFunctions();

    fboSize_ = size;
    fboFormat_ = format;

    // 设置目标帧率
    averageFrameInterval_ = avgFrameInterval;

    // 为每个buffer创建FBO
    for (auto &buffer : buffers_) {
        buffer->fbo = createFbo(size, format);
        if (!buffer->fbo || !buffer->fbo->isValid()) {
            qWarning() << QStringLiteral("[RenderBufferQueue] Failed to create FBO");
            cleanup();
            return false;
        }
        buffer->reset(this);
    }

    initialized_ = true;
    return true;
}

RenderBuffer *RenderBufferQueue::acquireForRender(int waitTimeoutMs)
{
    QMutexLocker locker(&mutex_);

    if (!initialized_) {
        return nullptr;
    }

    const qint64 currentTime = globalTimer_.elapsed();

    // 智能fence检查 - 根据帧率和时间动态调整
    const qint64 timeSinceLastCheck = currentTime - lastFenceCheckTime_;
    const int dynamicInterval =
        qBound(kMinFenceCheckInterval, static_cast<int>(averageFrameInterval_ / 6),
               kMaxFenceCheckInterval);

    if (timeSinceLastCheck >= dynamicInterval) {
        processPendingReleases();
        updateFenceStatus(true);
        processOutdatedFrames();
        lastFenceCheckTime_ = currentTime;
    }

    // 查找空闲的buffer
    RenderBuffer *availableBuffer = nullptr;
    for (auto &buffer : buffers_) {
        if (!buffer->inUse.load() && !buffer->displaying.load() && !buffer->ready.load() &&
            !buffer->pendingRelease.load()) {
            availableBuffer = buffer.get();
            break;
        }
    }

    // 如果没有可用buffer且允许等待
    if (!availableBuffer && waitTimeoutMs > 0) {
        if (bufferAvailable_.wait(&mutex_, waitTimeoutMs)) {
            // 重新查找
            for (auto &buffer : buffers_) {
                if (!buffer->inUse.load() && !buffer->displaying.load() && !buffer->ready.load() &&
                    !buffer->pendingRelease.load()) {
                    availableBuffer = buffer.get();
                    break;
                }
            }
        }
    }

    if (availableBuffer) {
        availableBuffer->inUse.store(true);
        availableBuffer->frameIndex = ++frameCounter_;
        availableBuffer->renderTime = currentTime;
        return availableBuffer;
    }

    // 没有可用的buffer，记录丢帧
    droppedFrameCount_.fetch_add(1);

    // 添加详细的跳帧输出
#if RENDER_BUFFER_QUEUE_DEBUG
    qWarning() << QStringLiteral(
                      "[RenderBufferQueue] 渲染跳帧 - 无可用Buffer | "
                      "帧序号: %1 | 等待时间: %2ms | 当前时间: %3ms | "
                      "总丢帧数: %4 | 平均帧间隔: %5ms")
                      .arg(frameCounter_ + 1)
                      .arg(waitTimeoutMs)
                      .arg(currentTime)
                      .arg(droppedFrameCount_.load())
                      .arg(averageFrameInterval_, 0, 'f', 2);
#endif

#if RENDER_BUFFER_QUEUE_DEBUG
    // 输出当前所有buffer状态用于调试
    QString bufferStatus;
    for (size_t i = 0; i < buffers_.size(); ++i) {
        const auto &buffer = buffers_[i];
        bufferStatus +=
            QStringLiteral(
                "Buffer[%1]: inUse=%2 displaying=%3 ready=%4 pending=%5 outdated=%6 frameIdx=%7; ")
                .arg(i)
                .arg(buffer->inUse.load())
                .arg(buffer->displaying.load())
                .arg(buffer->ready.load())
                .arg(buffer->pendingRelease.load())
                .arg(buffer->outdated.load())
                .arg(buffer->frameIndex);
    }
    qDebug() << QStringLiteral("[RenderBufferQueue] Buffer状态: %1").arg(bufferStatus);
#endif

    return nullptr;
}

void RenderBufferQueue::markRenderFinished(RenderBuffer *buffer, GLsync fence)
{
    if (!buffer) {
        return;
    }

    QMutexLocker locker(&mutex_);

    // 清理旧的fence
    if (buffer->fence) {
        glDeleteSync(buffer->fence);
    }

    buffer->fence = fence;
    buffer->inUse.store(false);
    buffer->renderTime = globalTimer_.elapsed();

    // 如果不支持fence，直接标记为ready
    if (!fence) {
        buffer->ready.store(true);

        // 当新帧ready时，检查是否需要丢弃更老的ready帧
        dropOlderReadyFrames(buffer->frameIndex);
    }

    // 通知等待的渲染线程
    bufferAvailable_.wakeOne();
}

RenderBuffer *RenderBufferQueue::acquireForDisplay()
{
    QMutexLocker locker(&mutex_);

    updateFenceStatus(true);

    // 状态验证
    validateBufferStates();

    // 处理待释放的buffer
    processPendingReleases();

    const qint64 currentTime = globalTimer_.elapsed();
    const qint64 currentDisplayFrameIndex =
        lastDisplayBuffer_ ? lastDisplayBuffer_->frameIndex : -1;

    // 策略1：优先寻找连续帧
    RenderBuffer *nextConsecutiveBuffer = nullptr;
    RenderBuffer *bestAlternativeBuffer = nullptr;

    qint64 targetFrameIndex = currentDisplayFrameIndex + 1;
    qint64 minFrameGap = INT_MAX;

    for (auto &buffer : buffers_) {
        if (buffer->ready.load() && !buffer->displaying.load() && !buffer->inUse.load() &&
            !buffer->pendingRelease.load() && !buffer->outdated.load()) {
            // 优先选择连续帧
            if (buffer->frameIndex == targetFrameIndex) {
                nextConsecutiveBuffer = buffer.get();
                break;
            }

            // 备选：选择最接近的较新帧
            if (buffer->frameIndex > currentDisplayFrameIndex) {
                int gap = buffer->frameIndex - targetFrameIndex;
                if (gap < minFrameGap) {
                    bestAlternativeBuffer = buffer.get();
                    minFrameGap = gap;
                }
            }
        }
    }

    RenderBuffer *candidateBuffer =
        nextConsecutiveBuffer ? nextConsecutiveBuffer : bestAlternativeBuffer;

    if (!candidateBuffer) {
        return nullptr;
    }

    // 策略2：更智能的切换条件
    bool shouldSwitch = true;

    if (lastDisplayBuffer_ && lastDisplayBuffer_->displaying.load()) {
        const qint64 bufferAge = currentTime - lastDisplayBuffer_->displayStartTime;
        const bool isConsecutiveFrame = (candidateBuffer->frameIndex == targetFrameIndex);

        // 新的切换条件（更宽松）：
        // 1. 连续帧
        // 3. 当前buffer太老（超过平均显示时间）
        // 4. 跳帧间隔太大（超过5帧）需要立即切换
        const bool bufferTooOld = (bufferAge > averageFrameInterval_);
        const bool gapTooLarge = (minFrameGap > 5);

        shouldSwitch = isConsecutiveFrame || bufferTooOld || gapTooLarge;

        if (!shouldSwitch) {
#if RENDER_BUFFER_QUEUE_DEBUG
            // 记录保持当前buffer的原因
            qDebug() << QStringLiteral(
                            "[RenderBufferQueue] 保持当前Buffer | "
                            "当前帧: %1 | 候选帧: %2 | Buffer年龄: %3ms | "
                            "连续帧: %4")
                            .arg(currentDisplayFrameIndex)
                            .arg(candidateBuffer->frameIndex)
                            .arg(bufferAge)
                            .arg(isConsecutiveFrame ? "是" : "否");
#endif
            return nullptr;
        }

        // 记录切换信息
#if RENDER_BUFFER_QUEUE_DEBUG
        qInfo() << QStringLiteral(
                       "[RenderBufferQueue] 切换显示Buffer | "
                       "旧帧: %1 → 新帧: %2 | Buffer年龄: %3ms | "
                       "连续帧: %4 | 跳帧数: %5")
                       .arg(currentDisplayFrameIndex)
                       .arg(candidateBuffer->frameIndex)
                       .arg(bufferAge)
                       .arg(isConsecutiveFrame ? "是" : "否")
                       .arg(isConsecutiveFrame ? 0 : minFrameGap);
#endif

        // 立即释放旧buffer
        lastDisplayBuffer_->displaying.store(false);
        lastDisplayBuffer_->pendingRelease.store(true);
    }

    // 切换到新buffer
    candidateBuffer->displaying.store(true);
    candidateBuffer->displayStartTime = currentTime;
    lastDisplayTime_ = currentTime;
    lastDisplayBuffer_ = candidateBuffer;
    candidateBuffer->outdated.store(false);

    // 延迟清理：只在必要时清理
    smartCleanupIfNeeded();

    return candidateBuffer;
}

void RenderBufferQueue::releaseDisplayBuffer(RenderBuffer *buffer)
{
    if (!buffer) {
        return;
    }

    QMutexLocker locker(&mutex_);

    // 简化释放逻辑，直接标记为待释放
    if (buffer->displaying.load()) {
#if RENDER_BUFFER_QUEUE_DEBUG
        qInfo() << QStringLiteral("[RenderBufferQueue] 标记Buffer待释放 | 帧序号: %1")
                       .arg(buffer->frameIndex);
#endif

        buffer->displaying.store(false);
        buffer->pendingRelease.store(true);

        // 如果这是当前显示buffer，清除引用
        if (lastDisplayBuffer_ == buffer) {
            lastDisplayBuffer_ = nullptr;
        }
    }
}

void RenderBufferQueue::cleanup()
{
    QMutexLocker locker(&mutex_);

    for (auto &buffer : buffers_) {
        if (buffer) {
            buffer->reset(this);
            buffer->fbo.reset();
        }
    }

    lastDisplayBuffer_ = nullptr;
    initialized_ = false;
}

RenderBufferQueue::Statistics RenderBufferQueue::getStatistics() const
{
    QMutexLocker locker(&mutex_);

    Statistics stats;
    stats.totalBuffers = static_cast<int>(buffers_.size());
    stats.droppedFrames = droppedFrameCount_.load();
    stats.outdatedFrames = outdatedFrameCount_.load();
    stats.averageFps = averageFrameInterval_ > 0 ? 1000.0 / averageFrameInterval_ : 0.0;

    for (const auto &buffer : buffers_) {
        if (buffer->inUse.load()) {
            stats.renderingBuffers++;
        } else if (buffer->displaying.load()) {
            stats.displayingBuffers++;
        } else if (buffer->ready.load()) {
            stats.readyBuffers++;
        } else if (buffer->pendingRelease.load()) {
            stats.pendingReleaseBuffers++;
        } else {
            stats.availableBuffers++;
        }
    }

    return stats;
}

void RenderBufferQueue::updateFenceStatus(bool forceCheck)
{
    bool hasActiveFences = false;

    for (auto &buffer : buffers_) {
        if (buffer->fence && !buffer->ready.load() && !buffer->inUse.load()) {
            hasActiveFences = true;

            GLenum status;
            if (forceCheck) {
                status =
                    glClientWaitSync(buffer->fence, GL_SYNC_FLUSH_COMMANDS_BIT, kFenceMicroWaitNs);
            } else {
                status = glClientWaitSync(buffer->fence, 0, 0);
            }

            if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
                buffer->ready.store(true);
                glDeleteSync(buffer->fence);
                buffer->fence = nullptr;

                bufferAvailable_.wakeOne();
            } else if (status == GL_WAIT_FAILED) {
                buffer->ready.store(true);
                glDeleteSync(buffer->fence);
                buffer->fence = nullptr;
                bufferAvailable_.wakeOne();
            }
        }
    }

    if (!hasActiveFences) {
        lastFenceCheckTime_ = globalTimer_.elapsed() + 2;
    }
}

void RenderBufferQueue::processPendingReleases()
{
    // 注意：此函数在mutex保护下调用

    qint64 currentTime = globalTimer_.elapsed();
    int releasedCount = 0;

    for (auto &buffer : buffers_) {
        if (buffer->pendingRelease.load()) {
            // 清理fence
            if (buffer->fence) {
                glDeleteSync(buffer->fence);
                buffer->fence = nullptr;
            }

            buffer->ready.store(false);
            buffer->displaying.store(false);
            buffer->pendingRelease.store(false);
            buffer->outdated.store(false);

            releasedCount++;

            // 通知等待的渲染线程
            bufferAvailable_.wakeOne();
        }
    }

    if (releasedCount > 0) {
#if RENDER_BUFFER_QUEUE_DEBUG
        qInfo()
            << QStringLiteral("[RenderBufferQueue] 释放了 %1 个待释放Buffer").arg(releasedCount);
#endif
    }
}

void RenderBufferQueue::processOutdatedFrames()
{
    // 注意：此函数在mutex保护下调用

    qint64 currentTime = globalTimer_.elapsed();

    for (auto &buffer : buffers_) {
        if (buffer->ready.load() && !buffer->displaying.load() && !buffer->inUse.load() &&
            !buffer->pendingRelease.load()) {
            // 只标记真正太老的buffer
            qint64 bufferAge = currentTime - buffer->renderTime;
            if (bufferAge > kForceCleanupInterval * 1.5) {
                // 但是如果这是唯一的ready buffer，不要标记为过时
                int readyBufferCount = 0;
                for (const auto &b : buffers_) {
                    if (b->ready.load() && !b->displaying.load() && !b->inUse.load() &&
                        !b->pendingRelease.load() && !b->outdated.load()) {
                        readyBufferCount++;
                    }
                }

                // 只有在有多个ready buffer时才标记过时
                if (readyBufferCount > 1) {
                    buffer->outdated.store(true);
                    outdatedFrameCount_.fetch_add(1);

                    // 添加过时帧输出
#if RENDER_BUFFER_QUEUE_DEBUG
                    qInfo() << QStringLiteral(
                                   "[RenderBufferQueue] 帧标记为过时 | "
                                   "帧序号: %1 | 帧年龄: %2ms | "
                                   "当前时间: %3ms | Ready帧数: %4 | "
                                   "总过时帧数: %5")
                                   .arg(buffer->frameIndex)
                                   .arg(bufferAge)
                                   .arg(currentTime)
                                   .arg(readyBufferCount)
                                   .arg(outdatedFrameCount_.load());
#endif
                }
            }
        }
    }
}

QSharedPointer<QOpenGLFramebufferObject> RenderBufferQueue::createFbo(
    const QSize &size, const QOpenGLFramebufferObjectFormat &format)
{
    if (!size.isValid()) {
        return nullptr;
    }
    return QSharedPointer<QOpenGLFramebufferObject>::create(size, format);
}

void RenderBufferQueue::dropOlderReadyFrames(int thresholdFrameIndex)
{
    // 注意：此函数在mutex保护下调用
    int droppedCount = 0;

#if RENDER_BUFFER_QUEUE_DEBUG
    QStringList droppedFrames;
#endif

    for (auto &buffer : buffers_) {
        // 只丢弃比阈值更老的ready帧
        if (buffer->ready.load() && !buffer->displaying.load() && !buffer->inUse.load() &&
            !buffer->pendingRelease.load() && buffer->frameIndex < thresholdFrameIndex) {
#if RENDER_BUFFER_QUEUE_DEBUG
            // 记录要丢弃的帧信息
            droppedFrames << QStringLiteral("帧%1(年龄:%2ms)")
                                 .arg(buffer->frameIndex)
                                 .arg(globalTimer_.elapsed() - buffer->renderTime);
#endif
            droppedCount++;

            // 清理fence
            if (buffer->fence) {
                glDeleteSync(buffer->fence);
                buffer->fence = nullptr;
            }

            // 重置buffer状态
            buffer->ready.store(false);
            buffer->outdated.store(false);

            // 统计丢弃的帧
            droppedFrameCount_.fetch_add(1);

            // 通知等待的渲染线程
            bufferAvailable_.wakeOne();
        }
    }

    // 输出丢弃帧的详细信息
    if (droppedCount > 0) {
#if RENDER_BUFFER_QUEUE_DEBUG
        qWarning() << QStringLiteral(
                          "[RenderBufferQueue] 清理老帧 | "
                          "阈值帧序号: %1 | 丢弃帧数: %2 | "
                          "丢弃的帧: [%3] | 总丢帧数: %4")
                          .arg(thresholdFrameIndex)
                          .arg(droppedCount)
                          .arg(droppedFrames.join(", "))
                          .arg(droppedFrameCount_.load());
#endif
    }
}

void RenderBufferQueue::validateBufferStates() const
{
    int displayingCount = 0;
    for (const auto &buffer : buffers_) {
        if (buffer->displaying.load()) {
            displayingCount++;
        }
    }

    if (displayingCount > 1) {
#if RENDER_BUFFER_QUEUE_DEBUG
        qWarning() << QStringLiteral(
                          "[RenderBufferQueue] Buffer状态异常 - 发现 %1 个displaying状态的buffer")
                          .arg(displayingCount);
#endif
    }
}

void RenderBufferQueue::smartCleanupIfNeeded()
{
    // 统计buffer状态
    int readyCount = 0;
    int oldFrameCount = 0;
    int currentDisplayFrame = lastDisplayBuffer_ ? lastDisplayBuffer_->frameIndex : -1;
    const int totalBuffers = static_cast<int>(buffers_.size());

    for (const auto &buffer : buffers_) {
        if (buffer->ready.load() && !buffer->displaying.load() && !buffer->inUse.load() &&
            !buffer->pendingRelease.load()) {
            readyCount++;

            // 定义"老帧"：比当前显示帧老
            if (currentDisplayFrame >= 0 && buffer->frameIndex < currentDisplayFrame) {
                oldFrameCount++;
            }
        }
    }

    // 清理条件：
    // 存在老帧，且数量接近一半（地板除）
    const bool tooManyOldFrames = oldFrameCount > (totalBuffers / 2);

    if (tooManyOldFrames) {
#if RENDER_BUFFER_QUEUE_DEBUG
        qInfo() << QStringLiteral(
                       "[RenderBufferQueue] 触发智能清理 | "
                       "Ready帧数: %1/%2 | 老帧数: %3 | "
                       "当前显示帧: %4 | 清理原因: %5")
                       .arg(readyCount)
                       .arg(totalBuffers)
                       .arg(oldFrameCount)
                       .arg(currentDisplayFrame)
                       .arg(QStringLiteral("老帧过多"));
#endif

        // 只清理真正老的帧
        dropOlderReadyFrames(currentDisplayFrame);
    }
}