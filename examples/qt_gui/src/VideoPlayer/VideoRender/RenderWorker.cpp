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

    context_->doneCurrent();
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
            render_->initialize(frame);
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
