#ifndef RENDERBUFFERQUEUE_H
#define RENDERBUFFERQUEUE_H

#include <QDebug>
#include <QElapsedTimer>
#include <QMutex>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QSharedPointer>
#include <QWaitCondition>
#include <atomic>
#include <vector>

/**
 * @brief 渲染缓冲区，表示一个带状态的FBO缓冲区
 */
struct RenderBuffer {
    QSharedPointer<QOpenGLFramebufferObject> fbo;
    GLsync fence = nullptr;                  // 当前写入操作的同步对象
    std::atomic<bool> ready{false};          // 是否已经完成GPU渲染
    std::atomic<bool> inUse{false};          // 是否被渲染线程占用
    std::atomic<bool> displaying{false};     // 是否正在被显示线程使用
    qint64 frameIndex = -1;                  // 帧索引，用于调试
    std::atomic<bool> pendingRelease{false}; // 是否等待释放
    qint64 renderTime = 0;                   // 渲染完成时间戳
    qint64 displayStartTime = 0;             // 开始显示的时间戳
    std::atomic<bool> outdated{false};       // 是否已过时（用于智能丢帧）

    RenderBuffer() = default;

    // 重置缓冲区状态
    void reset(QOpenGLExtraFunctions *func)
    {
        if (fence && func) {
            func->glDeleteSync(fence);
            fence = nullptr;
        }
        ready.store(false);
        inUse.store(false);
        displaying.store(false);
        pendingRelease.store(false);
        outdated.store(false);
        frameIndex = -1;
        renderTime = 0;
        displayStartTime = 0;
    }
};

/**
 * @brief 循环缓冲渲染队列
 */
class RenderBufferQueue : protected QOpenGLExtraFunctions {
public:
    explicit RenderBufferQueue(int bufferCount = 3);
    ~RenderBufferQueue();

    /**
     * @brief 初始化缓冲队列
     * @param size FBO的大小
     * @param format FBO的格式
     * @param avgFrameInterval 帧平均时长
     * @return 是否成功
     */
    bool initialize(const QSize &size,
                    const QOpenGLFramebufferObjectFormat &format = QOpenGLFramebufferObjectFormat(),
                    double avgFrameInterval = 40.0);

    /**
     * @brief 渲染线程获取一个空闲buffer用于渲染
     * @param waitTimeoutMs 等待超时时间（毫秒），0表示不等待
     * @return 可用的buffer指针，如果没有可用buffer则返回nullptr
     */
    RenderBuffer *acquireForRender(int waitTimeoutMs = 0);

    /**
     * @brief 渲染线程标记渲染完成，设置fence
     * @param buffer 渲染完成的buffer
     * @param fence OpenGL fence对象
     */
    void markRenderFinished(RenderBuffer *buffer, GLsync fence);

    /**
     * @brief 主线程获取一个已完成渲染的buffer用于显示
     * @return 可显示的buffer指针，如果没有可用buffer则返回nullptr
     */
    RenderBuffer *acquireForDisplay();

    /**
     * @brief 主线程释放显示buffer（智能延迟释放）
     * @param buffer 要释放的buffer
     */
    void releaseDisplayBuffer(RenderBuffer *buffer);

    /**
     * @brief 清理所有资源
     */
    void cleanup();

    /**
     * @brief 统计信息
     */
    struct Statistics {
        // 总帧数
        int totalBuffers = 0;
        // 有效帧数
        int availableBuffers = 0;
        // 渲染帧数
        int renderingBuffers = 0;
        // 准备好的帧数
        int readyBuffers = 0;
        // 展示帧数
        int displayingBuffers = 0;
        // 丢弃帧数
        int droppedFrames = 0;
        // 延迟释放帧数
        int pendingReleaseBuffers = 0;
        // 超时帧数
        int outdatedFrames = 0;
        // 平均帧率
        double averageFps = 0.0;
    };

    /**
     * @brief 获取统计信息
     * @return 统计信息结构体
     */
    Statistics getStatistics() const;

private:
    /**
     * @brief 智能fence检查，支持微阻塞等待
     */
    void updateFenceStatus(bool forceCheck = false);

    /**
     * @brief 智能处理延迟释放的buffer
     */
    void processPendingReleases();

    /**
     * @brief 智能丢帧策略
     */
    void processOutdatedFrames();

    /**
     * @brief 创建一个FBO
     */
    QSharedPointer<QOpenGLFramebufferObject> createFbo(
        const QSize &size, const QOpenGLFramebufferObjectFormat &format);

    /**
     * @brief 丢弃比指定帧索引更老的ready帧
     * @param currentFrameIndex 当前帧索引，用于确保丢弃的帧是更老的
     */
    void dropOlderReadyFrames(int currentFrameIndex);

    /**
     * @brief 验证Buffer状态是否有效
     *
     */
    void validateBufferStates() const;

    /**
     * @brief 按需清理Buffer的函数
     */
    void smartCleanupIfNeeded();

private:
    // 用于等待可用buffer
    mutable QMutex mutex_;
    QWaitCondition bufferAvailable_;

    // 缓冲区列表
    std::vector<std::unique_ptr<RenderBuffer>> buffers_;
    QSize fboSize_;
    QOpenGLFramebufferObjectFormat fboFormat_;

    // 是否初始化完成
    bool initialized_ = false;

    // 统计信息
    mutable std::atomic<int> droppedFrameCount_{0};
    mutable std::atomic<int> outdatedFrameCount_{0};
    qint64 frameCounter_ = 0;

    // 最后显示的buffer，避免过早释放
    RenderBuffer *lastDisplayBuffer_ = nullptr;

    // 智能优化相关
    QElapsedTimer globalTimer_;           // 全局计时器
    qint64 lastFenceCheckTime_ = 0;       // 上次fence检查时间
    double averageFrameInterval_ = 16.67; // 平均帧间隔(ms)，初始60fps
    qint64 lastDisplayTime_ = 0;          // 上次显示时间
};

#endif // RENDERBUFFERQUEUE_H