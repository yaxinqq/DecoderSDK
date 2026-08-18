#include "Renderer.h"
#include "AudioRender.h"
#include "DisplayRenderer.h"
#include "SnapShotVector.h"
#include "SoftwareRender.h"
#include "Stream.h"
#include "StreamTimeController.h"
#include "VideoPlayerImpl.h"
#include "VideoRender.h"

#include "decodersdk/common_define.h"

#ifdef CUDA_AVAILABLE
#include "Nv12Render_Cuda.h"
#endif

#ifdef D3D11VA_AVAILABLE
#include "Nv12Render_D3d11va.h"
#endif

#ifdef DXVA2_AVAILABLE
#include "Nv12Render_Dxva2.h"
#endif

#ifdef VAAPI_AVAILABLE
#include "Nv12Render_Vaapi.h"
#endif

#ifdef VULKAN_AVAILABLE
#include "Nv12Render_Vulkan.h"
#endif

#ifdef QSV_AVAILABLE
#include "mfxstructures.h"
#endif

#ifdef AMF_AVAILABLE
#include "AMF/Core/Surface.h"
#endif

#include <QByteArray>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QOpenGLContext>
#include <QSharedPointer>
#include <QThread>
#include <QTimer>

#include <memory>

namespace {
// bc6df5c5cc4e48c49232c3e6a1c52738
constexpr std::array<uint8_t, 16> kClippingStitchUuidBinary = {
    0xBC, 0x6D, 0xF5, 0xC5, 0xCC, 0x4E, 0x48, 0xC4, 0x92, 0x32, 0xC3, 0xE6, 0xA1, 0xC5, 0x27, 0x38};
} // namespace

#pragma region Renderer
class Renderer : public QObject {
    Q_OBJECT

public:
    // surface由主线程创建、也由主线程销毁；context也属于主线程，用来设置共享
    Renderer(QSurface *surface, QOpenGLContext *context, QObject *parent = nullptr);
    ~Renderer();

    /**
     * @brief 开启渲染（线程安全，可直接调用）
     */
    void start();
    /**
     * @brief 停止渲染（线程安全，可直接调用）
     */
    void stop();

    /**
     * @brief 设置音量 (0.0 - 1.0)（线程安全，可直接调用）
     */
    void setVolume(qreal volume);
    /**
     * @brief 获取音量（线程安全，可直接调用）
     */
    qreal volume() const;

    /**
     * @brief 重置时间轴（线程安全，通常在跳转或重连时调用）
     */
    void resetTimeline();
    /**
     * @brief 设置播放倍速（线程安全）
     * @param speed 倍速
     */
    void setSpeed(double speed);
    /**
     * @brief 设置主时钟类型（线程安全）
     * @param type 时钟类型
     */
    void setClockSourceType(Stream::ClockSourceType type);

signals:
    /**
     * @brief 渲染器名称改变信号
     *
     * @param rendererName 渲染器名称
     */
    void renderNameChanged(const QString &rendererName);
    /**
     * @brief 视频纹理准备就绪信号
     *
     * @param videoFrameParam 视频帧参数
     */
    void textureReady(const Stream::VideoFrameParam &videoFrameParam);

    /**
     * @brief 显示渲染器准备就绪信号
     *
     * @param playerId 玩家ID
     * @param renderer 显示渲染器
     */
    void displayRendererReady(const QString &playerId, std::weak_ptr<DisplayRenderer> renderer);

public slots:
    /**
     * @brief 设置当前渲染器对应的解码器
     *
     * @param decoder 解码器
     */
    void setDecoder(QPointer<IDecodeWorker> decoder);

    /**
     * @brief 添加显示渲染器
     *
     * @param playerId 玩家ID
     */
    void addDisplayRenderer(const QString &playerId);
    /**
     * @brief 移除显示渲染器
     *
     * @param playerId 玩家ID
     */
    void removeDisplayRenderer(const QString &playerId);

private slots:
    /**
     * @brief 尝试取出一帧进行渲染
     */
    void tryRenderFrame();

private:
    /**
     * @brief 渲染音频
     *
     * @param audioFrame 音频帧
     * @return 是否完成渲染
     */
    bool renderAudio(const decoder_sdk::Frame &audioFrame);
    /**
     * @brief 渲染视频
     *
     * @param videoFrame 视频帧
     * @return 是否完成渲染
     */
    bool renderVideo(const decoder_sdk::Frame &videoFrame);

    /**
     * @brief 处理视频帧中的SEI数据
     *
     * @param videoFrame 视频帧
     */
    void handleFrameSEI(const decoder_sdk::Frame &videoFrame);

    /**
     * @brief 创建视频渲染器
     *
     * @param videoFrame 视频帧
     * @return QSharedPointer<VideoRender> 视频渲染器
     */
    QSharedPointer<VideoRender> createRenderer(const decoder_sdk::Frame &videoFrame);

    /**
     * @brief 确保上下文已是当前上下文
     *
     * @return true 成功
     */
    bool ensureContextCurrent();
    /**
     * @brief 释放当前上下文
     */
    void releaseContextCurrent();

    /**
     * @brief 停止并释放音频渲染器
     */
    void stopAndReleaseAudioRender();

    /**
     * @brief 释放视频渲染器
     */
    void releaseVideoRender();

    /**
     * @brief 清空所有的展示渲染器
     */
    void clearAllDisplayRenderers();

    /**
     * @brief 从音频渲染设备更新音频时钟
     */
    void updateAudioClockFromRenderDevice();
    /**
     * @brief 安排下一次渲染任务
     * @param delayMs 延迟毫秒数
     */
    void scheduleNextRender(int delayMs = 0);
    /**
     * @brief 尝试处理音频输出
     * @return 是否成功处理
     */
    bool tryProcessAudio();
    /**
     * @brief 尝试处理视频渲染
     * @return 是否成功处理
     */
    bool tryProcessVideo();

private:
    // 渲染表面
    QSurface *surface_;
    // 渲染上下文
    QOpenGLContext *context_;

    // 所对应的解码器
    QPointer<IDecodeWorker> decoder_;

    // 是否启动渲染
    std::atomic_bool isStarted_;
    // 循环取帧时的，空闲避退定时器
    QTimer *backoffTimer_ = nullptr;

    // 视频部分
    // 视频渲染器
    QSharedPointer<VideoRender> render_;
    // 视频帧参数（最终结果，会考虑裁剪拼接）
    Stream::VideoFrameParam videoFrameParam_;
    // 视频帧原始宽度
    int frameOriginWidth_ = 0;
    // 视频帧原始高度
    int frameOriginHeight_ = 0;
    // 视频帧图像格式
    decoder_sdk::ImageFormat currentPixelFormat_ = decoder_sdk::ImageFormat::kUnknown;
    // 视频SEI解析出错信息次数
    int errorCount_ = 0;

    // 时间控制部分
    std::unique_ptr<StreamTimeController> timeController_;

    // 音频部分
    // 音频渲染器
    std::unique_ptr<AudioRender> audioRender_;
    // 音频参数缓存，用于判断是否需要重新初始化
    int audioSampleRate_ = 0;
    int audioChannels_ = 0;
    decoder_sdk::AudioSampleFormat audioSampleFormat_ = decoder_sdk::AudioSampleFormat::kUnknown;

    // VideoPlayerImpl对应的DisplayRenderer
    SnapshotVector<DisplayRenderer> displayRenderers_;
};

Renderer::Renderer(QSurface *surface, QOpenGLContext *context, QObject *parent)
    : QObject(parent), surface_(surface), isStarted_(false)
{
    // 外部已保证context是有效值
    context_ = new QOpenGLContext(this);
    context_->setFormat(context->format());
    context_->setShareContext(context);
    if (!context_->create()) {
        qWarning() << QStringLiteral("[Renderer] Failed to create OpenGL context.");
    }

    // 创建定时器，超时后，则将渲染任务加入队列
    backoffTimer_ = new QTimer(this);
    backoffTimer_->setSingleShot(true);
    backoffTimer_->setInterval(1);
    connect(backoffTimer_, &QTimer::timeout, this, &Renderer::tryRenderFrame, Qt::QueuedConnection);

    // 初始化时间控制器
    timeController_ = std::make_unique<StreamTimeController>();
}

Renderer::~Renderer()
{
    // 释放视频渲染器
    releaseVideoRender();

    // 停止并释放音频渲染器
    stopAndReleaseAudioRender();

    // 清空所有的展示渲染器
    clearAllDisplayRenderers();

    // 释放当前上下文
    releaseContextCurrent();
}

void Renderer::setDecoder(QPointer<IDecodeWorker> decoder)
{
    decoder_ = decoder;
}

void Renderer::start()
{
    if (isStarted_.load())
        return;

    isStarted_.store(true);

    // 将渲染任务加入队列，渲染资源懒加载
    QMetaObject::invokeMethod(this, &Renderer::tryRenderFrame, Qt::QueuedConnection);
}

void Renderer::stop()
{
    if (!isStarted_.load())
        return;

    isStarted_.store(false);
}

void Renderer::setVolume(qreal volume)
{
    if (!audioRender_)
        return;

    audioRender_->setVolume(volume);
}

qreal Renderer::volume() const
{
    return audioRender_ ? audioRender_->volume() : 0.0;
}

void Renderer::resetTimeline()
{
    if (timeController_) {
        timeController_->reset();
    }

    if (audioRender_) {
        audioRender_->flush();
    }
}

void Renderer::setSpeed(double speed)
{
    if (timeController_) {
        timeController_->setSpeed(speed);
    }
}

void Renderer::setClockSourceType(Stream::ClockSourceType type)
{
    if (timeController_) {
        timeController_->setClockSourceType(type);
    }
}

void Renderer::addDisplayRenderer(const QString &playerId)
{
    if (displayRenderers_.containsIf([&playerId](std::shared_ptr<DisplayRenderer> renderer) {
            return renderer && renderer->id() == playerId;
        }))
        return;

    // 确保当前context有效
    if (!ensureContextCurrent())
        return;

    // 创建新的DisplayRenderer，并缓存
    std::shared_ptr<DisplayRenderer> displayRenderer(new DisplayRenderer(playerId));
    displayRenderer->initialize();

    displayRenderers_.add(displayRenderer);

    // 设置player的displayRenderer
    emit displayRendererReady(playerId, displayRenderer);
}

void Renderer::removeDisplayRenderer(const QString &playerId)
{
    // 确保当前context有效
    ensureContextCurrent();
    displayRenderers_.removeIf([&playerId](std::shared_ptr<DisplayRenderer> renderer) {
        return renderer && renderer->id() == playerId;
    });
}

void Renderer::tryRenderFrame()
{
    // 1. 检查是否已启动渲染
    if (!isStarted_.load()) {
        // 清空资源并重置时间轴
        if (render_ && ensureContextCurrent()) {
            render_->uninitialize();
        }
        stopAndReleaseAudioRender();
        timeController_->reset();
        return;
    }

    // 2. 检查解码器是否有效
    if (!decoder_) {
        return;
    }

    // 3. 采样音频设备播放头，更新音频时钟
    updateAudioClockFromRenderDevice();

    // 4. 处理音频输出
    bool audioProcessed = tryProcessAudio();

    // 5. 处理视频渲染调度
    bool videoProcessed = tryProcessVideo();

    // 6. 安排下一轮调度
    // 如果处理了音频或视频，则立即进入下一轮
    // 如果都没有处理（包括视频等待中），则进行 2ms 高频轮询
    if (audioProcessed || videoProcessed) {
        scheduleNextRender(0);
    } else if (!backoffTimer_->isActive()) {
        scheduleNextRender(2);
    }
}

bool Renderer::tryProcessAudio()
{
    const double frontPts = decoder_->frontPts(decoder_sdk::MediaType::kAudio);
    const int bytesFree =
        audioRender_ ? audioRender_->bytesFree() : 65536; // 如果未初始化，假定空间充足

    // 调用时间控制器做调度判定
    auto decision = timeController_->decideAudio(frontPts, bytesFree);

    switch (decision.action) {
        case Stream::ScheduleAction::kRenderNow: {
            decoder_sdk::Frame audioFrame;
            if (decoder_->tryPopFrame(decoder_sdk::MediaType::kAudio, audioFrame)) {
                return renderAudio(audioFrame);
            }
            break;
        }
        case Stream::ScheduleAction::kWait: {
            // 音频过早，等待
            return false;
        }
        case Stream::ScheduleAction::kDrop: {
            // 虽然音频通常不丢弃，但如果调度器决定丢弃，则弹出并忽略
            decoder_sdk::Frame droppedFrame;
            if (decoder_->tryPopFrame(decoder_sdk::MediaType::kAudio, droppedFrame)) {
                qDebug() << QStringLiteral("[Renderer] Audio frame dropped! PTS:")
                         << droppedFrame.secPts();
                return true;
            }
            break;
        }
        case Stream::ScheduleAction::kNoClockReady: {
            // 时钟未就绪（如在视频主控模式下等待视频首帧）
            return false;
        }
    }

    return false;
}

bool Renderer::tryProcessVideo()
{
    const double frontPts = decoder_->frontPts(decoder_sdk::MediaType::kVideo);

    // 调用时间控制器做调度判定
    auto decision = timeController_->decideVideo(frontPts);

    switch (decision.action) {
        case Stream::ScheduleAction::kRenderNow: {
            decoder_sdk::Frame renderFrame;
            if (decoder_->tryPopFrame(decoder_sdk::MediaType::kVideo, renderFrame)) {
                bool ok = renderVideo(renderFrame);
                if (ok) {
                    timeController_->onVideoRendered(renderFrame.secPts());
                }
                return true;
            }
            break;
        }
        case Stream::ScheduleAction::kWait: {
            // 视频超前，不进行延迟等待，直接返回 false 让主循环进入轮询
            // 这样可以确保同线程的音频输出不会因为视频等待而断节奏
            return false;
        }
        case Stream::ScheduleAction::kDrop: {
            decoder_sdk::Frame droppedFrame;
            if (decoder_->tryPopFrame(decoder_sdk::MediaType::kVideo, droppedFrame)) {
                qDebug() << QStringLiteral("[Renderer] Video frame dropped! PTS:")
                         << droppedFrame.secPts();
                return true;
            }
            break;
        }
        case Stream::ScheduleAction::kNoClockReady: {
            // 时钟未就绪，通常在 AudioMaster 模式下等待音频首帧
            return false;
        }
    }

    return false;
}

void Renderer::updateAudioClockFromRenderDevice()
{
    if (audioRender_ && timeController_) {
        timeController_->updateAudioHardwareProgress(audioRender_->processedUSecs());
    }
}

void Renderer::scheduleNextRender(int delayMs)
{
    if (delayMs <= 0) {
        QMetaObject::invokeMethod(this, &Renderer::tryRenderFrame, Qt::QueuedConnection);
    } else {
        backoffTimer_->start(delayMs);
    }
}

bool Renderer::renderAudio(const decoder_sdk::Frame &audioFrame)
{
    if (!audioFrame.isValid() ||
        audioFrame.mediaType() != decoder_sdk::MediaType::kAudio) {
        return false;
    }

    const int sampleRate = audioFrame.sampleRate();
    const int channels = audioFrame.channels();
    const auto sampleFormat = audioFrame.sampleFormat();

    // 检查是否需要重新初始化音频渲染器
    bool needRecreateAudioRenderer = false;

    if (!audioRender_) {
        needRecreateAudioRenderer = true;
    } else if (audioSampleRate_ != sampleRate || audioChannels_ != channels ||
               audioSampleFormat_ != sampleFormat) {
        needRecreateAudioRenderer = true;
    }

    if (needRecreateAudioRenderer) {
        stopAndReleaseAudioRender();

        // 初始化
        audioRender_.reset(new AudioRender);
        connect(audioRender_.get(), &AudioRender::firstFrameRendered, this,
                [this](double pts, qint64 hardwareUSecs) {
                    if (timeController_) {
                        timeController_->setAudioClockBase(pts, hardwareUSecs);
                    }
                });
        audioRender_->initialize(audioFrame);
        audioSampleRate_ = sampleRate;
        audioChannels_ = channels;
        audioSampleFormat_ = sampleFormat;

        qDebug() << QStringLiteral("[Renderer] Initialized audio renderer - Sample Rate:")
                 << sampleRate << QStringLiteral(" Channels:") << channels
                 << QStringLiteral(" Format:") << static_cast<int>(sampleFormat);

        // 如果当前处于播放状态，立即启动音频
        audioRender_->start();
    }

    // 渲染音频帧（只有在准备好渲染时才处理）
    if (audioRender_) {
        audioRender_->render(audioFrame);
    }

    return true;
}

bool Renderer::renderVideo(const decoder_sdk::Frame &videoFrame)
{
    if (!videoFrame.isValid() ||
        videoFrame.mediaType() != decoder_sdk::MediaType::kVideo) {
        return false;
    }

    // 确保上下文是当前上下文
    if (!ensureContextCurrent()) {
        return false;
    }

    const auto width = videoFrame.width();
    const auto height = videoFrame.height();
    const auto pixelFormat = videoFrame.pixelFormat();

    // 检查是否需要重新创建视频渲染器
    bool needRecreateRenderer = false;

    if (!render_ || render_->shouldRebuild()) {
        needRecreateRenderer = true;
    } else if (frameOriginWidth_ != width || frameOriginHeight_ != height) {
        needRecreateRenderer = true;
    } else if (currentPixelFormat_ != pixelFormat) {
        // 像素格式改变，需要重新创建渲染器
        needRecreateRenderer = true;
    }

    if (needRecreateRenderer) {
        if (render_) {
            render_.reset();
        }

        // 根据像素格式创建新的渲染器
        render_ = createRenderer(videoFrame);
        if (render_) {
            frameOriginWidth_ = videoFrame.width();
            frameOriginHeight_ = videoFrame.height();
            currentPixelFormat_ = pixelFormat;
        } else {
            qWarning() << QStringLiteral(
                              "[Renderer] Failed to create video renderer for pixel format: %1")
                              .arg(QString::number(static_cast<int>(pixelFormat)));
            return false;
        }

        emit renderNameChanged(render_->renderName());
    }

    if (render_) {
        // 未初始化时，进行初始化
        if (!render_->isInitialized()) {
            render_->initialize(videoFrame);
        }

        // 处理SEI数据
        handleFrameSEI(videoFrame);
        render_->render(videoFrame, *displayRenderers_.getSnapshot(), &videoFrameParam_);
        emit textureReady(videoFrameParam_);
    }

    return true;
}

void Renderer::handleFrameSEI(const decoder_sdk::Frame &videoFrame)
{
    // 如果不是关键帧，则不处理
    if (videoFrame.keyFrame() != 1)
        return;

    // 遍历UUID，如果未找到对应的，也不进行处理
    const auto &seiDataList = videoFrame.userSEIDataList();
    for (int i = 0; i < seiDataList.size(); ++i) {
        // handle sei
    }

    // 如果SEI为空，则清空数据
    if (render_->hasClippingStitchRegions() && seiDataList.empty()) {
        render_->clearClippingStitchRegions();
    }
}

QSharedPointer<VideoRender> Renderer::createRenderer(const decoder_sdk::Frame &videoFrame)
{
    const auto format = videoFrame.pixelFormat();
    switch (format) {
#ifdef CUDA_AVAILABLE
        case decoder_sdk::ImageFormat::kCuda:
            return QSharedPointer<VideoRender>(new Nv12Render_Cuda);
#endif
#ifdef D3D11VA_AVAILABLE
        case decoder_sdk::ImageFormat::kD3d11va:
            return QSharedPointer<VideoRender>(new Nv12Render_D3d11va);
#endif
#ifdef DXVA2_AVAILABLE
        case decoder_sdk::ImageFormat::kDxva2:
            return QSharedPointer<VideoRender>(new Nv12Render_Dxva2);
#endif
#ifdef VAAPI_AVAILABLE
        case decoder_sdk::ImageFormat::kVaapi:
            return QSharedPointer<VideoRender>(new Nv12Render_Vaapi(context_));
#endif
#ifdef VULKAN_AVAILABLE
        case decoder_sdk::ImageFormat::kVulkan:
            return QSharedPointer<VideoRender>(new Nv12Render_Vulkan);
#endif
#ifdef QSV_AVAILABLE
        case decoder_sdk::ImageFormat::kQsv: {
#if defined(Q_OS_WIN)
            if (videoFrame.backendHwType() == decoder_sdk::HWAccelType::kD3d11va) {
                return QSharedPointer<VideoRender>(new Nv12Render_D3d11va);
            } else {
                return QSharedPointer<VideoRender>(new Nv12Render_Dxva2);
            }
#else
            return QSharedPointer<VideoRender>(new Nv12Render_Vaapi(context_));
#endif
        }
#endif
#ifdef AMF_AVAILABLE
        case decoder_sdk::ImageFormat::kAmf: {
#if defined(Q_OS_WIN)
            // 判断当前的解码后端是D3D11Va还是Dxva2
            if (videoFrame.backendHwType() == decoder_sdk::HWAccelType::kD3d11va) {
                return QSharedPointer<VideoRender>(new Nv12Render_D3d11va);
            } else {
                return QSharedPointer<VideoRender>(new Nv12Render_Dxva2);
            }
#else
            return nullptr;
#endif
        }
#endif
        default:
            // 对于软解格式，使用软解渲染器作为默认选择
            return QSharedPointer<VideoRender>(new SoftwareRender);
    }
}

bool Renderer::ensureContextCurrent()
{
    if (QOpenGLContext::currentContext() == context_) {
        return true;
    }

    if (!context_->makeCurrent(surface_)) {
        qWarning() << QStringLiteral("[Renderer] Failed to make OpenGL context current.");
        return false;
    }
    return true;
}

void Renderer::releaseContextCurrent()
{
    if (context_ && QOpenGLContext::currentContext() == context_) {
        context_->doneCurrent();
    }
}

void Renderer::stopAndReleaseAudioRender()
{
    if (audioRender_) {
        audioRender_->stop();
        audioRender_.reset(nullptr);
    }
    audioSampleRate_ = 0;
    audioChannels_ = 0;
    audioSampleFormat_ = decoder_sdk::AudioSampleFormat::kUnknown;
}

void Renderer::releaseVideoRender()
{
    // 释放视频渲染器
    if (render_) {
        // 确保上下文是当前上下文
        if (ensureContextCurrent()) {
            render_.reset(nullptr);
        }
    }

    currentPixelFormat_ = decoder_sdk::ImageFormat::kUnknown;
    frameOriginWidth_ = 0;
    frameOriginHeight_ = 0;
}

void Renderer::clearAllDisplayRenderers()
{
    if (displayRenderers_.empty() || !ensureContextCurrent())
        return;

    displayRenderers_.clear();
}

#pragma endregion

//
#pragma region RendererHelper
class RendererHelper : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

signals:
    /**
     * @brief 设置当前渲染器对应的解码器
     *
     * @param decoder 解码器
     */
    void requestToSetDecoder(QPointer<IDecodeWorker> decoder);

    /**
     * @brief 添加显示渲染器
     *
     * @param playerId 玩家ID
     */
    void requestToAddDisplayRenderer(const QString &playerId);
    /**
     * @brief 移除显示渲染器
     *
     * @param playerId 玩家ID
     */
    void requestToRemoveDisplayRenderer(const QString &playerId);

    /**
     * @brief 重置时间轴
     */
    void requestToResetTimeline();
    /**
     * @brief 设置倍速
     * @param speed 倍速
     */
    void requestToSetSpeed(double speed);
    /**
     * @brief 设置时钟类型
     * @param type 时钟类型
     */
    void requestToSetClockSourceType(Stream::ClockSourceType type);
};
#pragma endregion

//
#pragma region RenderWorker
RenderWorker::RenderWorker(QSurface *surface, const QString &key, QObject *parent)
    : QObject(parent), helper_{new RendererHelper(this)}, surface_(surface), key_(key)
{
}

RenderWorker::~RenderWorker()
{
    // if (renderer_) {
    //     renderer_->deleteLater();
    // }

    if (thread_ && thread_->isRunning()) {
        thread_->requestInterruption();
        thread_->quit();
        thread_->wait();
    }

    // 加断言，确保renderer_在析构时被删除
    Q_ASSERT(renderer_.isNull());
}

void RenderWorker::start()
{
    if (!renderer_) {
        initialize();
    }

    renderer_->start();
}

void RenderWorker::stop()
{
    if (!renderer_) {
        return;
    }

    renderer_->stop();
}

void RenderWorker::resetTimeline()
{
    emit helper_->requestToResetTimeline();
}

void RenderWorker::setClockSourceType(Stream::ClockSourceType type)
{
    clockSourceType_ = type;
    if (renderer_) {
        emit helper_->requestToSetClockSourceType(type);
    }
}

void RenderWorker::setSpeed(double speed)
{
    speed_ = speed;
    if (renderer_) {
        emit helper_->requestToSetSpeed(speed);
    }
}

void RenderWorker::setVolume(qreal volume)
{
    volume_ = volume;
    if (renderer_) {
        renderer_->setVolume(volume);
    }
}

qreal RenderWorker::volume() const
{
    return volume_;
}

void RenderWorker::setDecoder(QPointer<IDecodeWorker> decoder)
{
    decoder_ = decoder;

    emit helper_->requestToSetDecoder(decoder);
}

void RenderWorker::addDisplayRenderer(const QString &playerId)
{
    if (!renderer_) {
        initialize();
    }

    emit helper_->requestToAddDisplayRenderer(playerId);
}

void RenderWorker::removeDisplayRenderer(const QString &playerId)
{
    if (!renderer_) {
        return;
    }

    emit displayRendererAboutToDestroy(playerId);
    emit helper_->requestToRemoveDisplayRenderer(playerId);
}

void RenderWorker::initialize()
{
    std::call_once(initFlag_, [this]() {
        renderer_ = new Renderer(surface_, QOpenGLContext::globalShareContext());
        renderer_->setDecoder(decoder_);

        // 应用懒赋值的初始状态
        renderer_->setVolume(volume_);
        renderer_->setSpeed(speed_);
        renderer_->setClockSourceType(clockSourceType_);

        thread_ = new QThread(this);
        if (!key_.isEmpty()) {
            thread_->setObjectName(QStringLiteral("render_%1").arg(key_));
        }
        renderer_->moveToThread(thread_);
        thread_->start();

        // ============== helper => Renderer的信号 ============== //
        // 设置Decoder时，阻塞进行设置
        connect(helper_, &RendererHelper::requestToSetDecoder, renderer_.data(),
                &Renderer::setDecoder, Qt::BlockingQueuedConnection);

        connect(helper_, &RendererHelper::requestToAddDisplayRenderer, renderer_.data(),
                &Renderer::addDisplayRenderer);
        connect(helper_, &RendererHelper::requestToRemoveDisplayRenderer, renderer_.data(),
                &Renderer::removeDisplayRenderer);

        connect(helper_, &RendererHelper::requestToResetTimeline, renderer_.data(),
                &Renderer::resetTimeline);
        connect(helper_, &RendererHelper::requestToSetSpeed, renderer_.data(), &Renderer::setSpeed);
        connect(helper_, &RendererHelper::requestToSetClockSourceType, renderer_.data(),
                &Renderer::setClockSourceType);
        // ===================================================== //

        // ============== Renderer => Worker的信号 ============== //
        connect(renderer_.data(), &Renderer::renderNameChanged, this,
                &RenderWorker::renderNameChanged);
        connect(renderer_.data(), &Renderer::textureReady, this, &RenderWorker::textureReady);

        connect(renderer_.data(), &Renderer::displayRendererReady, this,
                &RenderWorker::displayRendererReady);
        // ===================================================== //

        // ============ 线程析构时，同时析构renderer ============ //
        connect(thread_, &QThread::finished, renderer_, &Renderer::deleteLater);
        // ===================================================== //
    });
}

#pragma endregion

#include "Renderer.moc"