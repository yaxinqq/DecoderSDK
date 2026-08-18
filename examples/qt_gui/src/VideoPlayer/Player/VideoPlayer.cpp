#include "VideoPlayer.h"
#include "CommonUtils.h"
#include "RubberBand.h"
#include "StreamManager.h"
#include "VideoPlayerImpl.h"
#include "VideoRender.h"
#include "../Base/InternalUtils.h"

#include <QApplication>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>

namespace {
    constexpr int kRubberBandValidLength = 10;

    void adjustRectBoundary(QRectF &rect)
    {
        rect.setLeft(std::clamp(rect.left(), 0.0, 1.0));
        rect.setTop(std::clamp(rect.top(), 0.0, 1.0));
        rect.setWidth(std::clamp(rect.width(), 0.0, 1.0 - rect.left()));
        rect.setHeight(std::clamp(rect.height(), 0.0, 1.0 - rect.top()));
    }

    void adjustRectWidthOrHeight(QRectF &rect, double minRectSize)
    {
        const QPointF center = rect.center();
        const auto width = std::max(std::max(rect.width(), rect.height()), minRectSize);

        rect.setSize(QSizeF(width, width));
        rect.moveCenter(center);

        if (utils::greater(rect.right(), 1.0)) {
            rect.translate(1.0 - rect.right(), 0);
        }
        if (utils::greater(rect.bottom(), 1.0)) {
            rect.translate(0, 1.0 - rect.bottom());
        }
    }
} // namespace

VideoPlayer::VideoPlayer(QWidget *parent)
    : QOpenGLWidget(parent)
{
    impl_ = new VideoPlayerImpl(this);
    maxDigitalZoomFactor_ = 99;
    rubberBand_.reset(new RubberBand(QRubberBand::Rectangle, this));
    rubberBand_->hide();

    connect(impl_, &VideoPlayerImpl::widgetRectChanged, this, &VideoPlayer::widgetRectChanged);
    connect(impl_, &VideoPlayerImpl::videoRectChanged, this, &VideoPlayer::videoRectChanged);
    connect(impl_, &VideoPlayerImpl::playerStateChanged, this, &VideoPlayer::playerStateChanged);
    connect(impl_, &VideoPlayerImpl::aboutToUpdate, this, &VideoPlayer::aboutToRenderFrame);
    connect(impl_, &VideoPlayerImpl::streamClosed, this, &VideoPlayer::streamClosed);
    connect(impl_, &VideoPlayerImpl::fileStreamLoopEnded, this, &VideoPlayer::fileStreamLoopEnded);
    connect(impl_, &VideoPlayerImpl::errorOccured, this, &VideoPlayer::errorOccured);
    connect(impl_, &VideoPlayerImpl::recordStarted, this, &VideoPlayer::recordStarted);
    connect(impl_, &VideoPlayerImpl::recordStopped, this, &VideoPlayer::recordStopped);
    connect(impl_, &VideoPlayerImpl::totalTimeRecved, this, &VideoPlayer::totalTimeRecved);
    connect(impl_, &VideoPlayerImpl::ptsChanged, this, &VideoPlayer::ptsChanged);
    connect(impl_, &VideoPlayerImpl::seekStarted, this, &VideoPlayer::seekStarted);
    connect(impl_, &VideoPlayerImpl::seekEnded, this, &VideoPlayer::seekEnded);
    connect(impl_, &VideoPlayerImpl::frameSizeChanged, this, &VideoPlayer::frameSizeChanged);

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

void VideoPlayer::renderToImage(QImage &image)
{
    return impl_->renderToImage(image);
}

void VideoPlayer::setDigitalZoomRect(const QRectF &rect)
{
    impl_->setDigitalZoomRect(rect);
}

QRectF VideoPlayer::digitalZoomRect() const
{
    return impl_->digitalZoomRect();
}

int VideoPlayer::maxDigitalZoomFactor() const
{
    return maxDigitalZoomFactor_;
}

void VideoPlayer::setMaxDigitalZoomFactor(int maxFactor)
{
    if (maxDigitalZoomFactor_ == maxFactor)
        return;

    maxDigitalZoomFactor_ = maxFactor;
    setNormalizedDigitalZoomRect(digitalZoomRect());
}

void VideoPlayer::setBlackRects(const QVector<QRect> &rects)
{
    blackRects_ = rects;
}

void VideoPlayer::setDigitalZoomControlEnabled(bool enabled)
{
    if (digitalZoomControlEnabled_ == enabled)
        return;

    digitalZoomControlEnabled_ = enabled;
    if (!digitalZoomControlEnabled_) {
        resetDigitalZoomInteractionState();
    }
}

bool VideoPlayer::isDigitalZoomControlEnabled() const
{
    return digitalZoomControlEnabled_;
}

void VideoPlayer::setHorizontalFlip(bool flip)
{
    impl_->setHorizontalFlip(flip);
}

bool VideoPlayer::isHorizontalFlip() const
{
    return impl_->isHorizontalFlip();
}

void VideoPlayer::setVecticalFlip(bool flip)
{
    impl_->setVecticalFlip(flip);
}

bool VideoPlayer::isVecticalFlip() const
{
    return impl_->isVecticalFlip();
}

void VideoPlayer::setHorizontalAndVecticalFlip(bool hflip, bool vflip)
{
    impl_->setHorizontalAndVecticalFlip(hflip, vflip);
}

QPair<bool, bool> VideoPlayer::isHorizontalAndVecticalFlip() const
{
    return impl_->isHorizontalAndVecticalFlip();
}

void VideoPlayer::setColorAdjustments(float brightness, float contrast, float saturation, float hue)
{
    impl_->setColorAdjustments(brightness, contrast, saturation, hue);
}

void VideoPlayer::setBrightness(float brightness)
{
    impl_->setBrightness(brightness);
}

float VideoPlayer::brightness() const
{
    return impl_->brightness();
}

void VideoPlayer::setContrast(float contrast)
{
    impl_->setContrast(contrast);
}

float VideoPlayer::contrast() const
{
    return impl_->contrast();
}

void VideoPlayer::setSaturation(float saturation)
{
    impl_->setSaturation(saturation);
}

float VideoPlayer::saturation() const
{
    return impl_->saturation();
}

void VideoPlayer::setHue(float hue)
{
    impl_->setHue(hue);
}

float VideoPlayer::hue() const
{
    return impl_->hue();
}

void VideoPlayer::startRecoding(const QString &recodDir)
{
    // do nothing
}

void VideoPlayer::stopRecoding()
{
    // do nothing
}

bool VideoPlayer::isRecording() const
{
    // do nothing
    return false;
}

QString VideoPlayer::defaultRecordFileName() const
{
    return impl_->id();
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

void VideoPlayer::mousePressEvent(QMouseEvent *event)
{
    const auto validRect = acceptedDigitalZoomResponseRect();
    if (!digitalZoomControlEnabled_ || !validRect.isValid() || !validRect.contains(event->pos())) {
        QOpenGLWidget::mousePressEvent(event);
        return;
    }

    if (inDigitalZoomBlackRect(event->pos())) {
        QOpenGLWidget::mousePressEvent(event);
        return;
    }

    if (event->button() == Qt::LeftButton) {
        leftBtnPressed_ = true;
        originPos_ = event->pos();
        if (rubberBand_) {
            rubberBand_->setGeometry(QRect(originPos_.value(), QSize()));
            rubberBand_->show();
        }
        event->accept();
        return;
    }

    if (event->button() == Qt::MiddleButton) {
        resetDigitalZoomRect();
        event->accept();
        return;
    }

    if (event->button() == Qt::RightButton) {
        rightBtnPressed_ = true;
        pressedPos_ = event->pos();
        event->accept();
        return;
    }

    // 默认情况
    QOpenGLWidget::mousePressEvent(event);
}

void VideoPlayer::mouseMoveEvent(QMouseEvent *event)
{
    if (!digitalZoomControlEnabled_) {
        QOpenGLWidget::mouseMoveEvent(event);
        return;
    }

    const auto validRect = acceptedDigitalZoomResponseRect();
    if (!validRect.isValid()) {
        QOpenGLWidget::mouseMoveEvent(event);
        return;
    }

    if (leftBtnPressed_ && rubberBand_ && originPos_.has_value() && (event->buttons() & Qt::LeftButton)) {
        QRect rubberBandRect(originPos_.value(), constrainPointToRect(event->pos(), validRect));
        rubberBandRect = rubberBandRect.normalized();
        if (!validRect.contains(rubberBandRect)) {
            rubberBandRect = validRect.intersected(rubberBandRect);
        }
        rubberBand_->setGeometry(rubberBandRect);
        event->accept();
        return;
    }

    if (rightBtnPressed_ && (event->buttons() & Qt::RightButton)) {
        moveDigitalZoomRect(pressedPos_.x() - event->x(), pressedPos_.y() - event->y());
        event->accept();
        return;
    }

    // 默认情况
    QOpenGLWidget::mouseMoveEvent(event);
}

void VideoPlayer::mouseReleaseEvent(QMouseEvent *event)
{
    // 鼠标松开时，清空 originPos_，防止被重复使用
    originPos_ = std::nullopt;

    if (!digitalZoomControlEnabled_) {
        QOpenGLWidget::mouseReleaseEvent(event);
        return;
    }

    // 鼠标松开时
    bool handleLeftBtnPressed = false;
    if (event->button() == Qt::LeftButton && leftBtnPressed_) {
        leftBtnPressed_ = false;
        handleLeftBtnPressed = true;
    }

    // 鼠标松开时
    bool handleRigthBtnPressed = false;
    if (event->button() == Qt::RightButton && rightBtnPressed_) {
        rightBtnPressed_ = false;
        handleRigthBtnPressed = true;
    }

    // 当前只处理鼠标左键按下的情况
    if (event->button() != Qt::LeftButton || !handleLeftBtnPressed) {
        QOpenGLWidget::mouseReleaseEvent(event);
        return;
    }

    // 获得当前可接受响应的区域
    const auto validRect = acceptedDigitalZoomResponseRect();
    if (!rubberBand_ || !validRect.isValid()) {
        QOpenGLWidget::mouseReleaseEvent(event);
        return;
    }

    rubberBand_->hide();

    if (rubberBand_->width() < kRubberBandValidLength && rubberBand_->height() < kRubberBandValidLength) {
        rubberBand_->setGeometry(0, 0, 0, 0);
        QOpenGLWidget::mouseReleaseEvent(event);
        return;
    }

    const QRect rubberBandRect = rubberBand_->geometry();
    const QRectF zoomRect = digitalZoomRect();
    const double dX = static_cast<double>(rubberBandRect.x() - validRect.x()) / validRect.width() * zoomRect.width() + zoomRect.x();
    const double dY = static_cast<double>(rubberBandRect.y() - validRect.y()) / validRect.height() * zoomRect.height() + zoomRect.y();
    const double dW = static_cast<double>(rubberBandRect.width()) / validRect.width() * zoomRect.width();
    const double dH = static_cast<double>(rubberBandRect.height()) / validRect.height() * zoomRect.height();
    setNormalizedDigitalZoomRect(QRectF(dX, dY, dW, dH));

    rubberBand_->setGeometry(0, 0, 0, 0);
    event->accept();
}

void VideoPlayer::mouseDoubleClickEvent(QMouseEvent *event)
{
    QOpenGLWidget::mouseDoubleClickEvent(event);
}

void VideoPlayer::wheelEvent(QWheelEvent *event)
{
    const auto validRect = acceptedDigitalZoomResponseRect();
    if (!digitalZoomControlEnabled_ || !validRect.isValid() || !validRect.contains(event->position().toPoint())) {
        QOpenGLWidget::wheelEvent(event);
        return;
    }

    if (inDigitalZoomBlackRect(event->position().toPoint())) {
        QOpenGLWidget::wheelEvent(event);
        return;
    }

    const auto scale = event->angleDelta().y() > 0 ? 1.0 / wheelFactor_ : wheelFactor_;
    setDigitalZoomScale(scale);
    event->accept();
}

void VideoPlayer::aboutToRenderFrame()
{
    needToRender_.store(true);
    update();
}

QRect VideoPlayer::acceptedDigitalZoomResponseRect() const
{
    const QRect responseRect = videoRect();
    if (responseRect.isValid() && !responseRect.isEmpty()) {
        return responseRect;
    }

    return {};
}

QPoint VideoPlayer::constrainPointToRect(const QPoint &point, const QRect &rect) const
{
    if (rect.contains(point))
        return point;

    const int x = qBound(rect.left(), point.x(), rect.right());
    const int y = qBound(rect.top(), point.y(), rect.bottom());
    return QPoint(x, y);
}

bool VideoPlayer::inDigitalZoomBlackRect(const QPoint &point) const
{
    for (const auto &blackRect : blackRects_) {
        if (blackRect.contains(point))
            return true;
    }

    return false;
}

void VideoPlayer::setNormalizedDigitalZoomRect(QRectF rect)
{
    adjustRectWidthOrHeight(rect, 1.0 / maxDigitalZoomFactor_);
    adjustRectBoundary(rect);
    setDigitalZoomRect(rect);
}

void VideoPlayer::setDigitalZoomScale(double scale)
{
    QRectF rect;
    const QRectF zoomRect = digitalZoomRect();
    const auto width = std::max(std::max(zoomRect.width() / scale, zoomRect.height() / scale), 1.0 / maxDigitalZoomFactor_);
    if (utils::greater(width, 1.0)) {
        rect = { 0.0, 0.0, 1.0, 1.0 };
    } else {
        rect.setSize(QSizeF(width, width));
        rect.moveCenter(zoomRect.center());

        if (utils::greater(rect.right(), 1.0)) {
            rect.translate(1.0 - rect.right(), 0);
        }
        if (utils::greater(rect.bottom(), 1.0)) {
            rect.translate(0, 1.0 - rect.bottom());
        }
    }

    adjustRectBoundary(rect);
    setDigitalZoomRect(rect);
}

void VideoPlayer::moveDigitalZoomRect(int xStep, int yStep)
{
    const auto validRect = acceptedDigitalZoomResponseRect();
    if (!validRect.isValid())
        return;

    const QRectF zoomRect = digitalZoomRect();
    double dX = zoomRect.x() + static_cast<double>(xStep) / validRect.width() * zoomRect.width();
    double dY = zoomRect.y() + static_cast<double>(yStep) / validRect.height() * zoomRect.height();

    if (dX > 1.0 - zoomRect.width())
        dX = 1.0 - zoomRect.width();
    if (dX < 0.0)
        dX = 0.0;
    if (dY > 1.0 - zoomRect.height())
        dY = 1.0 - zoomRect.height();
    if (dY < 0.0)
        dY = 0.0;

    setDigitalZoomRect(QRectF(dX, dY, zoomRect.width(), zoomRect.height()));
    pressedPos_ = pressedPos_ - QPoint(xStep, yStep);
}

void VideoPlayer::resetDigitalZoomRect()
{
    setDigitalZoomRect({ 0.0, 0.0, 1.0, 1.0 });
}

void VideoPlayer::resetDigitalZoomInteractionState()
{
    leftBtnPressed_ = false;
    rightBtnPressed_ = false;
    originPos_ = std::nullopt;
    if (rubberBand_) {
        rubberBand_->hide();
        rubberBand_->setGeometry(0, 0, 0, 0);
    }
}
