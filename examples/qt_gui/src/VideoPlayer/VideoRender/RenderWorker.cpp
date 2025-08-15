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

#include <QDebug>
#include <QOpenGLContext>
#include <QThread>

RenderWorker::RenderWorker(QSurface *surface, QOpenGLContext *context, QObject *parent)
    : QObject(parent), surface_(surface)
{
    readyRender_.store(false);

    context_ = new QOpenGLContext(this);
    context_->setFormat(context->format());
    context_->setShareContext(context);
    context_->create();
}

RenderWorker::~RenderWorker()
{
    if (render_) {
        context_->makeCurrent(surface_);
        render_.reset(nullptr);
    }

    if (audioRender_) {
        audioRender_->stop();
        audioRender_.reset(nullptr);
    }

    context_->doneCurrent();
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

void RenderWorker::render(const std::shared_ptr<decoder_sdk::Frame> &frame)
{
    if (!frame || !frame->isValid())
        return;

    // 根据帧类型进行不同的处理
    switch (frame->mediaType()) {
        case decoder_sdk::MediaType::kMediaTypeAudio:
            renderAudio(frame);
            // qInfo() << "Audio Pts: " << frame->secPts();
            break;
        case decoder_sdk::MediaType::kMediaTypeVideo:
            renderVideo(frame);
            // qInfo() << "Video Pts: " << frame->secPts();
            break;
        default:
            qWarning() << "[RenderWorker] Unsupported frame type received.";
            return;
    }
}

void RenderWorker::prepareStop()
{
    if (render_) {
        render_.reset(nullptr);
        context_->doneCurrent();
    }
    if (audioRender_) {
        audioRender_.reset(nullptr);
    }

    currentPixelFormat_ = decoder_sdk::ImageFormat::kUnknown;
    readyRender_.store(false);
}

void RenderWorker::preparePause()
{
    if (render_) {
        render_.reset(nullptr);
    }
    if (audioRender_) {
        audioRender_.reset(nullptr);
    }

    currentPixelFormat_ = decoder_sdk::ImageFormat::kUnknown;
    readyRender_.store(false);
}

void RenderWorker::preparePlaying()
{
    readyRender_.store(true);
}

void RenderWorker::renderAudio(const std::shared_ptr<decoder_sdk::Frame> &audioFrame)
{
    if (!audioFrame || !audioFrame->isValid() ||
        audioFrame->mediaType() != decoder_sdk::MediaType::kMediaTypeAudio) {
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
        if (audioRender_) {
            // 停止旧的音频渲染
            audioRender_->stop();
        }

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

void RenderWorker::renderVideo(const std::shared_ptr<decoder_sdk::Frame> &videoFrame)
{
    if (!videoFrame || !videoFrame->isValid() ||
        videoFrame->mediaType() != decoder_sdk::MediaType::kMediaTypeVideo) {
        return;
    }

    const auto width = videoFrame->width();
    const auto height = videoFrame->height();
    const auto pixelFormat = videoFrame->pixelFormat();

    // 检查是否需要重新创建视频渲染器
    bool needRecreateRenderer = false;

    if (!render_ || !render_->isValid()) {
        needRecreateRenderer = true;
    } else if (renderWidth_ != width || renderHeight_ != height) {
        needRecreateRenderer = true;
    } else if (currentPixelFormat_ != pixelFormat) {
        // 像素格式改变，需要重新创建渲染器
        needRecreateRenderer = true;
    }

    if (needRecreateRenderer) {
        context_->makeCurrent(surface_);

        // 释放旧的渲染器
        if (render_) {
            render_.reset();
        }

        // 根据像素格式创建新的渲染器
        render_ = createRenderer(pixelFormat);
        if (render_) {
            render_->initialize(videoFrame);
            renderWidth_ = width;
            renderHeight_ = height;
            currentPixelFormat_ = pixelFormat;
        } else {
            qWarning() << "[RenderWorker] Failed to create video renderer for pixel format:"
                       << static_cast<int>(pixelFormat);
            return;
        }
    }

    if (render_) {
        render_->render(videoFrame);
        emit textureReady(render_, videoFrame->secPts());
    }
}

QSharedPointer<VideoRender> RenderWorker::createRenderer(decoder_sdk::ImageFormat format)
{
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
        default:
            // 对于软解格式，使用软解渲染器作为默认选择
            return QSharedPointer<VideoRender>(new SoftwareRender);
    }
}
