#include "VideoPlayer.h"
#include "RenderWorker.h"
#include "VideoPlayerImpl.h"
#include "VideoRender.h"

#include <QApplication>
#include <QOffscreenSurface>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <QWindow>

VideoPlayer::VideoPlayer(QWidget *parent) : QOpenGLWidget(parent)
{
    impl_ = new VideoPlayerImpl(this);
    connect(impl_, &VideoPlayerImpl::widgetRectChanged, this, &VideoPlayer::widgetRectChanged);
    connect(impl_, &VideoPlayerImpl::videoRectChanged, this, &VideoPlayer::videoRectChanged);
    connect(impl_, &VideoPlayerImpl::playerStateChanged, this, &VideoPlayer::playerStateChanged);
    connect(impl_, &VideoPlayerImpl::aboutToUpdate, this, &VideoPlayer::aboutToRenderFrame);
    connect(impl_, &VideoPlayerImpl::streamClosed, this, &VideoPlayer::streamClosed);

    connect(this, &VideoPlayer::forceToRender, this, &VideoPlayer::aboutToRenderFrame);
}

VideoPlayer::~VideoPlayer()
{
}

Stream::PlayerState VideoPlayer::playerState() const
{
    return impl_->playerState();
}

Stream::AspectRatioMode VideoPlayer::aspectRatioMode() const
{
    return impl_->aspectRatioMode();
}

QRect VideoPlayer::widgetRect() const
{
    return impl_->widgetRect();
}

QRect VideoPlayer::videoRect() const
{
    return impl_->videoRect();
}

bool VideoPlayer::streamOpenTimeout() const
{
    return impl_->streamOpenTimeout();
}

void VideoPlayer::renderToImage(QImage &image)
{
    return impl_->renderToImage(image);
}

void VideoPlayer::setMasks(QList<QImage *> masks)
{
    impl_->setMasks(masks);
}

void VideoPlayer::setShownScreenText(const QString &shownScreenText)
{
    impl_->setShownScreenText(shownScreenText);
}

void VideoPlayer::setPlayerState(Stream::PlayerState state)
{
    impl_->setPlayerState(state);
}

void VideoPlayer::initializeGL()
{
    auto context = QOpenGLContext::currentContext();
    if (!context)
        return;

    impl_->initialize(context, context->surface());
}

void VideoPlayer::resizeGL(int w, int h)
{
    context()->functions()->glViewport(0, 0, w, h);

    impl_->resize(w, h);
}

void VideoPlayer::paintGL()
{
    const auto wgtRect = rect();
    impl_->paintGL(wgtRect, wgtRect, wgtRect.bottomLeft());
}

void VideoPlayer::paintEvent(QPaintEvent *e)
{
    QPainter painter(this);
    const auto wgtRect = rect();
    if (playerState() != Stream::PlayerState::Playing) {
        impl_->clearByPainter(&painter, wgtRect);
    }

    if (!needToRender_.load()) {
        impl_->paintCommon(&painter, wgtRect);
        return;
    }

    QOpenGLWidget::paintEvent(e);
    needToRender_.store(false);
    impl_->paintCommon(&painter, rect());
}

void VideoPlayer::aboutToRenderFrame()
{
    needToRender_.store(true);
    update();
}