#include "DigitalZoomController.h"

#include <QGraphicsView>
#include <QWidget>

#include <cmath>

namespace {
#define EPSILON 1e-6
#define DOUBLEEPSILON 1e-12

bool greater(double a, double b, double epsilon = DOUBLEEPSILON)
{
    return a > b && std::fabs(a - b) > epsilon;
}
bool greater(float a, float b, double epsilon = EPSILON)
{
    return a > b && std::fabs(a - b) > epsilon;
}

// 调整矩形区域，确保在0~1的范围内
void adjustRectBoundary(QRectF &rect)
{
    rect.setLeft(std::clamp(rect.left(), 0.0, 1.0));
    rect.setTop(std::clamp(rect.top(), 0.0, 1.0));
    rect.setWidth(std::clamp(rect.width(), 0.0, 1.0 - rect.left()));
    rect.setHeight(std::clamp(rect.height(), 0.0, 1 - rect.top()));
}

// 调整矩形区域的宽或高，需要受到最小尺寸的限制
void adjustRectWidthOrHeight(QRectF &rect, double minRectSize)
{
    const QPointF c = rect.center();
    const auto w = std::max(std::max(rect.width(), rect.height()), minRectSize);

    rect.setSize(QSizeF(w, w));
    rect.moveCenter(c);

    if (greater(rect.right(), 1.0)) {
        rect.translate(1.0 - rect.right(), 0);
    }
    if (greater(rect.bottom(), 1.0)) {
        rect.translate(0, 1.0 - rect.bottom());
    }
}
} // namespace

DigitalZoomController::DigitalZoomController(QObject *parent) : QObject(parent)
{
    maxDigitalZoomFactor_ = 10;
}

DigitalZoomController::~DigitalZoomController()
{
}

QRectF DigitalZoomController::zoomRect() const
{
    return zoomRect_;
}

void DigitalZoomController::setZoomRect(QRectF rect)
{
    // 根据宽度或是高度计算另一个
    adjustRectWidthOrHeight(rect, 1.0 / maxDigitalZoomFactor_);

    // 进行边界验证和收缩
    adjustRectBoundary(rect);

    if (rect == zoomRect_)
        return;

    zoomRect_ = rect;
    emit zoomRectChanged(rect);
}

void DigitalZoomController::setZoomScale(double scale)
{
    QRectF rect;
    // 根据scale和videoRatio计算新的宽高
    // 如果有任一边是 >= 1，则恢复原始状态
    const auto w = std::max(std::max(zoomRect_.width() / scale, zoomRect_.height() / scale),
                            1.0 / maxDigitalZoomFactor_);
    if (greater(w, 1.0)) {
        rect = {0.0, 0.0, 1.0, 1.0};
    } else {
        rect.setSize(QSizeF(w, w));
        rect.moveCenter(zoomRect_.center());

        if (greater(rect.right(), 1.0)) {
            rect.translate(1.0 - rect.right(), 0);
        }
        if (greater(rect.bottom(), 1.0)) {
            rect.translate(0, 1.0 - rect.bottom());
        }
    }

    // 进行边界验证和收缩
    adjustRectBoundary(rect);

    if (rect == zoomRect_)
        return;

    zoomRect_ = rect;
    emit zoomRectChanged(rect);
}

void DigitalZoomController::zoomRectMove(int xStep, int yStep)
{
    const auto validRect = acceptedResponseRect();

    double dX = zoomRect_.x() + static_cast<double>(xStep) / validRect.width() * zoomRect_.width();
    double dY =
        zoomRect_.y() + static_cast<double>(yStep) / validRect.height() * zoomRect_.height();

    if (dX > 1.0 - zoomRect_.width())
        dX = 1.0 - zoomRect_.width();
    if (dX < 0.0)
        dX = 0.0;
    if (dY > 1.0 - zoomRect_.height())
        dY = 1.0 - zoomRect_.height();
    if (dY < 0.0)
        dY = 0.0;

    setZoomRect(QRectF(dX, dY, zoomRect_.width(), zoomRect_.height()));
    pressedPos_ = (pressedPos_ - QPoint(xStep, yStep));
}

void DigitalZoomController::resetZoomRect()
{
    setZoomRect({0.0, 0.0, 1.0, 1.0});
}

void DigitalZoomController::setEnabled(bool enabled)
{
    enabled_ = enabled;
}

bool DigitalZoomController::isEnabled() const
{
    return enabled_;
}

int DigitalZoomController::maxDigitalZoomFactor() const
{
    return maxDigitalZoomFactor_;
}

void DigitalZoomController::setMaxDigitalZoomFactor(int maxFactor)
{
    if (maxDigitalZoomFactor_ == maxFactor)
        return;

    maxDigitalZoomFactor_ = maxFactor;
    setZoomRect(zoomRect_);
}

bool DigitalZoomController::eventFilter(QObject *watched, QEvent *event)
{
    if (QGraphicsView *const view = qobject_cast<QGraphicsView *>(activeWidget_);
        watched != activeWidget_ && view && watched != view->viewport())
        return QObject::eventFilter(watched, event);

    const auto eventType = event->type();
    if (!funcs_.contains(eventType))
        return QObject::eventFilter(watched, event);

    // 如果当前的事件不需要外部应用层处理，则返回 true
    if ((this->*funcs_.value(eventType))(event)) {
        return true;
    }

    return QObject::eventFilter(watched, event);
}

QRect DigitalZoomController::acceptedResponseRect() const
{
    if (!activeWidget_)
        return {};

    return activeWidget_->rect();
}

QPoint DigitalZoomController::constrainPointToRect(const QPoint &point, const QRect &rect) const
{
    if (rect.contains(point))
        return point;

    int x = qBound(rect.left(), point.x(), rect.right());
    int y = qBound(rect.top(), point.y(), rect.bottom());
    return QPoint(x, y);
}