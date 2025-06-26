#include "RenderWorker.h"
#include "Nv12Render_Cuda.h"
#include "Nv12Render_D3d11va.h"
#include "Nv12Render_Dxva2.h"

#include <QDebug>
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

    context_->doneCurrent();
}

QSharedPointer<VideoRender> RenderWorker::createRenderer(decoder_sdk::ImageFormat format)
{
    switch (format) {
        case decoder_sdk::ImageFormat::kCuda:
            return QSharedPointer<VideoRender>(new Nv12Render_Cuda);

        case decoder_sdk::ImageFormat::kD3d11va:
            return QSharedPointer<VideoRender>(new Nv12Render_D3d11va);

        case decoder_sdk::ImageFormat::kDxva2:
            return QSharedPointer<VideoRender>(new Nv12Render_Dxva2);

        // 对于软解格式，可以使用默认的CUDA渲染器或添加专门的软解渲染器
        case decoder_sdk::ImageFormat::kNV12:
        case decoder_sdk::ImageFormat::kNV21:
        case decoder_sdk::ImageFormat::kYUV420P:
        case decoder_sdk::ImageFormat::kYUV422P:
        case decoder_sdk::ImageFormat::kYUV444P:
            // 对于软解格式，使用CUDA渲染器作为默认选择
            return QSharedPointer<VideoRender>(new Nv12Render_Cuda);

        default:
            qWarning() << "Unsupported pixel format:" << static_cast<int>(format)
                       << ", using CUDA renderer as fallback";
            return QSharedPointer<VideoRender>(new Nv12Render_Cuda);
    }
}

void RenderWorker::render(const decoder_sdk::Frame &frame)
{
    if (!frame.isValid())
        return;

    const auto width = frame.width();
    const auto height = frame.height();
    const auto pixelFormat = frame.pixelFormat();

    // 检查是否需要重新创建渲染器
    bool needRecreateRenderer = false;

    if (!render_) {
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
            render_->initialize(width, height);
            renderWidth_ = width;
            renderHeight_ = height;
            currentPixelFormat_ = pixelFormat;

            qDebug() << "Created renderer for pixel format:" << static_cast<int>(pixelFormat)
                     << "size:" << width << "x" << height;
        } else {
            qWarning() << "Failed to create renderer for pixel format:"
                       << static_cast<int>(pixelFormat);
            return;
        }
    }

    if (render_) {
        render_->render(frame);
        emit textureReady(render_);
    }
}

void RenderWorker::prepareStop()
{
    if (render_) {
        render_.reset(nullptr);
        context_->doneCurrent();
    }
    currentPixelFormat_ = decoder_sdk::ImageFormat::kUnknown;
    readyRender_.store(false);
}

void RenderWorker::preparePause()
{
    if (render_) {
        render_.reset(nullptr);
    }
    currentPixelFormat_ = decoder_sdk::ImageFormat::kUnknown;
    readyRender_.store(false);
}

void RenderWorker::preparePlaying()
{
    readyRender_.store(true);
}
