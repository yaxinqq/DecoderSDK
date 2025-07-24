#include "RenderBufferQueue.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QOpenGLContext>
#include <QScreen>

#include <algorithm>

namespace {
// 流畅度优化参数
constexpr int kMinFenceCheckInterval = 1;  // 最小fence检查间隔(ms)
constexpr int kMaxFenceCheckInterval = 8;  // 最大fence检查间隔(ms)
constexpr int kFrameIntervalSamples = 60;  // 帧间隔采样数量
constexpr int kMaxBufferAgeMs = 100;       // buffer最大存活时间(ms)
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
    lastForceCleanupTime_ = globalTimer_.elapsed();

    // 尝试获取系统刷新率
    estimateDisplayRefreshRate();
}

RenderBufferQueue::~RenderBufferQueue()
{
    cleanup();
}

bool RenderBufferQueue::initialize(const QSize &size, const QOpenGLFramebufferObjectFormat &format,
                                   double targetFps)
{
    QMutexLocker locker(&mutex_);

    if (initialized_ || !size.isValid()) {
        return false;
    }

    initializeOpenGLFunctions();

    fboSize_ = size;
    fboFormat_ = format;

    // 设置目标帧率
    if (targetFps > 0) {
        averageFrameInterval_ = 1000.0 / targetFps;
    }

    // 为每个buffer创建FBO
    for (auto &buffer : buffers_) {
        buffer->fbo = createFbo(size, format);
        if (!buffer->fbo || !buffer->fbo->isValid()) {
            qWarning() << "[RenderBufferQueue] Failed to create FBO";
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
        updateFenceStatus();
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
    return nullptr;
}

RenderBuffer *RenderBufferQueue::acquireForDisplay()
{
    QMutexLocker locker(&mutex_);

    if (!initialized_) {
        return nullptr;
    }

    const qint64 currentTime = globalTimer_.elapsed();

    // 更新帧率统计
    if (lastFrameTime_ > 0 && frameIntervalSamples_ < kFrameIntervalSamples) {
        const qint64 frameInterval = currentTime - lastFrameTime_;
        if (frameInterval > 5 && frameInterval < 200) // 过滤异常值
        {
            averageFrameInterval_ =
                (averageFrameInterval_ * frameIntervalSamples_ + frameInterval) /
                (frameIntervalSamples_ + 1);
            frameIntervalSamples_++;
        }
    }
    lastFrameTime_ = currentTime;

    // 更积极的fence检查策略
    const qint64 timeSinceLastCheck = currentTime - lastFenceCheckTime_;
    bool shouldForceCheck = false;

    // 如果距离上次显示时间过长，强制检查
    if (lastDisplayTime_ > 0 && (currentTime - lastDisplayTime_) > averageFrameInterval_ * 1.2) {
        shouldForceCheck = true;
    }

    // 更频繁的检查，确保及时发现ready的buffer
    if (timeSinceLastCheck >= 1 || shouldForceCheck) {
        processPendingReleases();
        updateFenceStatus(shouldForceCheck);
        processOutdatedFrames();
        lastFenceCheckTime_ = currentTime;
    }

    // 获取当前显示帧的索引，用于确保帧序单调递增
    const int currentDisplayFrameIndex = lastDisplayBuffer_ && lastDisplayBuffer_->displaying.load()
                                             ? lastDisplayBuffer_->frameIndex
                                             : -1;

    // 立即丢弃所有比当前显示帧更老的ready帧
    dropOlderReadyFrames(currentDisplayFrameIndex);

    // 简化的buffer选择策略 - 严格保证帧序递增
    RenderBuffer *bestBuffer = nullptr;
    int bestFrameIndex = currentDisplayFrameIndex; // 确保只选择更新的帧

    // 寻找帧索引最小但大于当前显示帧的ready buffer（最接近的下一帧）
    for (auto &buffer : buffers_) {
        if (buffer->ready.load() && !buffer->displaying.load() && !buffer->inUse.load() &&
            !buffer->pendingRelease.load() && !buffer->outdated.load() &&
            buffer->frameIndex > currentDisplayFrameIndex) {
            // 选择帧索引最小的（最接近当前帧的下一帧）
            if (!bestBuffer || buffer->frameIndex < bestBuffer->frameIndex) {
                bestBuffer = buffer.get();
                bestFrameIndex = buffer->frameIndex;
            }
        }
    }

    // 检查是否需要切换到新buffer
    if (bestBuffer) {
        // 如果有当前显示的buffer，检查切换条件
        if (lastDisplayBuffer_ && lastDisplayBuffer_->displaying.load()) {
            const qint64 bufferAge = currentTime - lastDisplayBuffer_->displayStartTime;

            // 切换条件：
            // 1. 当前buffer显示时间超过平均帧间隔
            // 2. 或者当前buffer太老
            const bool shouldSwitch =
                (bufferAge >= averageFrameInterval_ * 0.8) || (bufferAge > kMaxBufferAgeMs);

            if (!shouldSwitch) {
                return nullptr; // 继续使用当前buffer
            }
        }

        // 切换到新buffer
        bestBuffer->displaying.store(true);
        bestBuffer->displayStartTime = currentTime;
        lastDisplayTime_ = currentTime;

        // 重置outdated标记
        bestBuffer->outdated.store(false);

        // 计算显示延迟
        if (latencySamples_ < 100) {
            const double latency = currentTime - bestBuffer->renderTime;
            averageDisplayLatency_ =
                (averageDisplayLatency_ * latencySamples_ + latency) / (latencySamples_ + 1);
            latencySamples_++;
        }

        return bestBuffer;
    }

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

void RenderBufferQueue::releaseDisplayBuffer(RenderBuffer *buffer)
{
    if (!buffer) {
        return;
    }

    QMutexLocker locker(&mutex_);

    // 智能延迟释放策略
    if (lastDisplayBuffer_ && lastDisplayBuffer_ != buffer) {
        // 检查是否应该立即释放旧buffer
        qint64 currentTime = globalTimer_.elapsed();
        qint64 bufferAge = currentTime - lastDisplayBuffer_->displayStartTime;

        // 如果旧buffer显示时间过长，立即释放
        if (bufferAge > averageFrameInterval_ * 2) {
            lastDisplayBuffer_->pendingRelease.store(true);
            lastDisplayBuffer_->displaying.store(false);
        } else {
            // 否则标记为待释放
            lastDisplayBuffer_->pendingRelease.store(true);
            lastDisplayBuffer_->displaying.store(false);
        }
    }

    lastDisplayBuffer_ = buffer;
}

void RenderBufferQueue::setDisplayRefreshRate(double refreshRate)
{
    QMutexLocker locker(&mutex_);
    if (refreshRate > 0) {
        displayRefreshRate_ = refreshRate;
        // 重新调整平均帧间隔的期望值
        if (frameIntervalSamples_ == 0) {
            const double expectedInterval = 1000.0 / refreshRate;
            averageFrameInterval_ = expectedInterval;
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
    frameIntervalSamples_ = 0;
    latencySamples_ = 0;
}

RenderBufferQueue::Statistics RenderBufferQueue::getStatistics() const
{
    QMutexLocker locker(&mutex_);

    Statistics stats;
    stats.totalBuffers = static_cast<int>(buffers_.size());
    stats.droppedFrames = droppedFrameCount_.load();
    stats.outdatedFrames = outdatedFrameCount_.load();
    stats.averageFps = averageFrameInterval_ > 0 ? 1000.0 / averageFrameInterval_ : 0.0;
    stats.displayLatency = averageDisplayLatency_;

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
    // 注意：此函数在mutex保护下调用

    bool hasActiveFences = false;

    for (auto &buffer : buffers_) {
        if (buffer->fence && !buffer->ready.load() && !buffer->inUse.load()) {
            hasActiveFences = true;

            // 智能fence检查：根据情况选择阻塞或非阻塞
            GLenum status;
            if (forceCheck) {
                // 强制检查时，使用微阻塞等待获得更准确的状态
                status =
                    glClientWaitSync(buffer->fence, GL_SYNC_FLUSH_COMMANDS_BIT, kFenceMicroWaitNs);
            } else {
                // 常规检查，非阻塞
                status = glClientWaitSync(buffer->fence, 0, 0);
            }

            if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
                // GPU渲染完成，标记为ready
                buffer->ready.store(true);

                // 立即清理fence以减少GPU内存占用
                glDeleteSync(buffer->fence);
                buffer->fence = nullptr;

                // 当新帧ready时，立即丢弃更老的ready帧
                dropOlderReadyFrames(buffer->frameIndex);

                // 通知等待的线程
                bufferAvailable_.wakeOne();
            } else if (status == GL_WAIT_FAILED) {
                // 出错时也标记为ready，避免死锁
                buffer->ready.store(true);
                glDeleteSync(buffer->fence);
                buffer->fence = nullptr;

                // 同样需要丢弃更老的帧
                dropOlderReadyFrames(buffer->frameIndex);

                bufferAvailable_.wakeOne();
            }
            // GL_TIMEOUT_EXPIRED 表示还未完成，继续等待
        }
    }

    // 如果没有活跃的fence，可以减少检查频率
    if (!hasActiveFences) {
        lastFenceCheckTime_ = globalTimer_.elapsed() + 2; // 延迟下次检查
    }
}

void RenderBufferQueue::processPendingReleases()
{
    // 注意：此函数在mutex保护下调用

    qint64 currentTime = globalTimer_.elapsed();

    // 定期强制清理
    bool forceCleanup = (currentTime - lastForceCleanupTime_) > kForceCleanupInterval;
    if (forceCleanup) {
        lastForceCleanupTime_ = currentTime;
    }

    for (auto &buffer : buffers_) {
        if (buffer->pendingRelease.load()) {
            // 智能释放策略：不是当前显示buffer或强制清理时才释放
            bool shouldRelease = (buffer.get() != lastDisplayBuffer_) || forceCleanup;

            if (shouldRelease) {
                // 清理fence
                if (buffer->fence) {
                    glDeleteSync(buffer->fence);
                    buffer->fence = nullptr;
                }

                buffer->ready.store(false);
                buffer->displaying.store(false);
                buffer->pendingRelease.store(false);
                buffer->outdated.store(false);

                // 通知等待的渲染线程
                bufferAvailable_.wakeOne();
            }
        }
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
                }
            }
        }
    }
}

void RenderBufferQueue::dropOlderReadyFrames(int currentFrameIndex)
{
    // 注意：此函数在mutex保护下调用

    for (auto &buffer : buffers_) {
        // 丢弃比当前帧更老的ready帧
        if (buffer->ready.load() && !buffer->displaying.load() && !buffer->inUse.load() &&
            !buffer->pendingRelease.load() && buffer->frameIndex < currentFrameIndex) {
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
}

void RenderBufferQueue::estimateDisplayRefreshRate()
{
    // 尝试从系统获取刷新率
    if (QGuiApplication::primaryScreen()) {
        double refreshRate = QGuiApplication::primaryScreen()->refreshRate();
        if (refreshRate > 0) {
            setDisplayRefreshRate(refreshRate);
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