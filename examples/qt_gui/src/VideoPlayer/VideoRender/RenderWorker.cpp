#include "RenderWorker.h"
#include "SoftwareRender.h"

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

#include <QDebug>
#include <QOpenGLContext>
#include <QThread>

RenderWorker::RenderWorker(QSurface *surface, QOpenGLContext *context, QObject *parent)
    : QObject(parent), surface_(surface)
{
    // 外部已保证context是有效值
    context_ = new QOpenGLContext(this);
    context_->setFormat(context->format());
    context_->setShareContext(context);
    if (!context_->create()) {
        qWarning() << "[RenderWorker] Failed to create OpenGL context.";
    }
}

RenderWorker::~RenderWorker()
{
    // 释放视频渲染器
    releaseVideoRender();

    // 停止并释放音频渲染器
    stopAndReleaseAudioRender();

    // 释放当前上下文
    releaseContextCurrent();
}

void RenderWorker::setVolume(qreal volume)
{
    if (!audioRender_)
        return;

    audioRender_->setVolume(volume);
}

qreal RenderWorker::volume() const
{
    return audioRender_ ? audioRender_->volume() : 0.0;
}

void RenderWorker::render(const std::shared_ptr<decoder_sdk::Frame> &frame,
                          const Stream::VideoProcessParam &processParam)
{
    if (!frame || !frame->isValid())
        return;

    // 根据帧类型进行不同的处理
    switch (frame->mediaType()) {
        case decoder_sdk::MediaType::kAudio:
            renderAudio(frame);
            // qInfo() << "Audio Pts: " << frame->secPts();
            break;
        case decoder_sdk::MediaType::kVideo:
            renderVideo(frame, processParam);
            // qInfo() << "Video Pts: " << frame->secPts();
            break;
        default:
            qWarning() << "[RenderWorker] Unsupported frame type received.";
            return;
    }
}

void RenderWorker::prepareStop()
{
    // 释放视频渲染器
    releaseVideoRender();

    // 停止并释放音频渲染器
    stopAndReleaseAudioRender();

    // 释放当前上下文
    releaseContextCurrent();
}

void RenderWorker::preparePause()
{
    // 视频渲染器只保留必要的资源
    if (render_) {
        if (ensureContextCurrent()) {
            render_->uninitialize();
        }
    }

    // 停止并释放音频渲染器
    stopAndReleaseAudioRender();
}

void RenderWorker::preparePlaying()
{
    // do nothing
}

void RenderWorker::renderAudio(const std::shared_ptr<decoder_sdk::Frame> &audioFrame)
{
    if (!audioFrame || !audioFrame->isValid() ||
        audioFrame->mediaType() != decoder_sdk::MediaType::kAudio) {
        return;
    }

    const int sampleRate = audioFrame->sampleRate();
    const int channels = audioFrame->channels();
    const auto sampleFormat = audioFrame->sampleFormat();

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
        audioRender_->initialize(audioFrame);
        audioSampleRate_ = sampleRate;
        audioChannels_ = channels;
        audioSampleFormat_ = sampleFormat;

        qDebug() << "[RenderWorker] Initialized audio renderer - Sample Rate:" << sampleRate
                 << "Channels:" << channels << "Format:" << static_cast<int>(sampleFormat);

        // 如果当前处于播放状态，立即启动音频
        audioRender_->start();
    }

    // 渲染音频帧（只有在准备好渲染时才处理）
    if (audioRender_) {
        audioRender_->render(audioFrame);
    }
}

void RenderWorker::renderVideo(const std::shared_ptr<decoder_sdk::Frame> &videoFrame,
                               const Stream::VideoProcessParam &processParam)
{
    if (!videoFrame || !videoFrame->isValid() ||
        videoFrame->mediaType() != decoder_sdk::MediaType::kVideo) {
        return;
    }

    // 确保上下文是当前上下文
    if (!ensureContextCurrent()) {
        return;
    }

    const auto width = videoFrame->width();
    const auto height = videoFrame->height();
    const auto pixelFormat = videoFrame->pixelFormat();

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
            frameOriginWidth_ = videoFrame->width();
            frameOriginHeight_ = videoFrame->height();
            currentPixelFormat_ = pixelFormat;
        } else {
            qWarning() << "[RenderWorker] Failed to create video renderer for pixel format:"
                       << static_cast<int>(pixelFormat);
            return;
        }
    }

    if (render_) {
        // 未初始化时，进行初始化
        if (!render_->isInitialized()) {
            render_->initialize(videoFrame, processParam);
        }

        // 处理SEI数据
        handleFrameSEI(videoFrame);
        render_->render(videoFrame, &videoFrameParam_);
        emit textureReady(render_, videoFrameParam_);
    }
}

void RenderWorker::handleFrameSEI(const std::shared_ptr<decoder_sdk::Frame> &videoFrame)
{
    // 如果不是关键帧，则不处理
    if (videoFrame->keyFrame() != 1)
        return;

    // 遍历UUID，如果未找到对应的，也不进行处理
    const auto &seiDataList = videoFrame->userSEIDataList();
    for (int i = 0; i < seiDataList.size(); ++i) {
        // do something
    }
}

QSharedPointer<VideoRender> RenderWorker::createRenderer(
    const std::shared_ptr<decoder_sdk::Frame> &videoFrame)
{
    const auto format = videoFrame->pixelFormat();
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
            if (videoFrame->backendHwType() == decoder_sdk::HWAccelType::kD3d11va) {
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
            if (videoFrame->backendHwType() == decoder_sdk::HWAccelType::kD3d11va) {
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

bool RenderWorker::ensureContextCurrent()
{
    if (QOpenGLContext::currentContext() == context_) {
        return true;
    }
    if (!context_->makeCurrent(surface_)) {
        qWarning() << QStringLiteral("[RenderWorker] Failed to make OpenGL context current.");
        return false;
    }
    return true;
}

void RenderWorker::releaseContextCurrent()
{
    if (context_ && QOpenGLContext::currentContext() == context_) {
        context_->doneCurrent();
    }
}

void RenderWorker::stopAndReleaseAudioRender()
{
    if (audioRender_) {
        audioRender_->stop();
        audioRender_.reset(nullptr);
    }
    audioSampleRate_ = 0;
    audioChannels_ = 0;
    audioSampleFormat_ = decoder_sdk::AudioSampleFormat::kUnknown;
}

void RenderWorker::releaseVideoRender()
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