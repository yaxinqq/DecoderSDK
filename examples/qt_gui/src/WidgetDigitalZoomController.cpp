#include "WidgetDigitalZoomController.h"

#include <QMouseEvent>
#include <QPainter>
#include <QRubberBand>
#include <QWheelEvent>

namespace {
const int kRubberBandValidLength = 10;

const QColor kColor = Qt::red;
const int kPenWidth = 4;
const double kBrushAlpha = 0.05;
} // namespace

#pragma retion RubberBand
class RubberBand : public QRubberBand {
public:
    explicit RubberBand(Shape s, QWidget *parent = nullptr);
    ~RubberBand();

protected:
    void paintEvent(QPaintEvent *event) override;
};

RubberBand::RubberBand(Shape s, QWidget *parent) : QRubberBand(QRubberBand::Rectangle, parent)
{
    setAttribute(Qt::WA_TranslucentBackground);
}

RubberBand::~RubberBand()
{
}

void RubberBand::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QPen pen = painter.pen();
    pen.setColor(kColor);
    pen.setStyle(Qt::SolidLine);
    pen.setWidth(kPenWidth);
    painter.setPen(pen);

    QColor brushColor = kColor;
    brushColor.setAlphaF(kBrushAlpha);

    QBrush brush = painter.brush();
    brush.setColor(brushColor);
    brush.setStyle(Qt::SolidPattern);
    painter.setBrush(brush);

    painter.drawRect(rect());
}
#pragma endregion

WidgetDigitalZoomController::WidgetDigitalZoomController(QWidget *parent)
    : DigitalZoomController(qobject_cast<QObject *>(parent))
{
    activeWidget_ = parent;
    initByActiveWidget(parent);

    // 如果传入了有效的 parent，就安装事件过滤器
    if (activeWidget_)
        activeWidget_->installEventFilter(this);
}

WidgetDigitalZoomController::~WidgetDigitalZoomController()
{
}

void WidgetDigitalZoomController::setActicedWidget(QWidget *widget)
{
    // 如果之前已有活跃的窗口，则解除事件过滤器
    if (activeWidget_) {
        activeWidget_->removeEventFilter(this);
    }

    activeWidget_ = widget;
    initByActiveWidget(activeWidget_);

    // 如果传入了有效的 parent，就安装事件过滤器
    if (activeWidget_)
        activeWidget_->installEventFilter(this);
}

void WidgetDigitalZoomController::setActiveWidgetLogicContentMargins(const QMargins &margins)
{
    logicContentMargins_ = margins;
}

QMargins WidgetDigitalZoomController::activeWidgetContentMargins() const
{
    return logicContentMargins_;
}

bool WidgetDigitalZoomController::handleMousePressEvent(QEvent *event)
{
    if (!activeWidget_ || !enabled_)
        return false;

    QMouseEvent *const mouseEvent = dynamic_cast<QMouseEvent *>(event);
    if (!mouseEvent)
        return false;

    // 如果不在可接受响应的区域，就返回
    const auto validRect = acceptedResponseRect();
    if (!validRect.contains(mouseEvent->pos())) {
        return false;
    }

    // 获取鼠标左键按下位置
    if (mouseEvent->button() == Qt::LeftButton) {
        leftBtnPressed_ = true;
        leftBtnPressedPos_ = mouseEvent->pos();
    }

    // 获取鼠标右键按下位置
    if (mouseEvent->button() == Qt::RightButton) {
        rightBtnPressed_ = true;
        rightBtnPressedPos_ = mouseEvent->pos();
    }

    // 如果画布漫游行为不是"禁用橡皮筋"，开始画框逻辑
    if (mouseEvent->button() == Qt::LeftButton) {
        originPos_ = mouseEvent->pos();
        if (rubberBand_) {
            rubberBand_->setGeometry(QRect(originPos_.value().toPoint(), QSize()));
            rubberBand_->setVisible(true);
        }
    }
    // 鼠标中键按下还原
    else if (mouseEvent->button() == Qt::MiddleButton) {
        resetZoomRect();
    }
    // 鼠标右键按下开始漫游逻辑
    else if (mouseEvent->button() == Qt::RightButton) {
        pressedPos_ = mouseEvent->pos();
    }

    return true;
}

bool WidgetDigitalZoomController::handleMouseMoveEvent(QEvent *event)
{
    if (!activeWidget_ || !enabled_)
        return false;

    QMouseEvent *const mouseEvent = dynamic_cast<QMouseEvent *>(event);
    if (!mouseEvent)
        return false;

    // 获得当前可接受响应的区域
    const auto validRect = acceptedResponseRect();
    // 将mouseEvent的pos约束在validRect中
    const auto validPt = constrainPointToRect(mouseEvent->pos(), validRect);

    // 执行画框逻辑
    if (mouseEvent->buttons() & Qt::LeftButton && rubberBand_ && originPos_.has_value()) {
        QRect rubberBandRect = QRect(originPos_.value().toPoint(), validPt).normalized();
        if (!validRect.contains(rubberBandRect)) {
            rubberBandRect = validRect.intersected(rubberBandRect);
        }
        rubberBand_->setGeometry(rubberBandRect);
    }
    // 执行漫游逻辑
    else if (mouseEvent->buttons() & Qt::RightButton && rightBtnPressed_) {
        zoomRectMove(pressedPos_.x() - mouseEvent->x(), pressedPos_.y() - mouseEvent->y());
    }

    return true;
}

bool WidgetDigitalZoomController::handleMouseReleaseEvent(QEvent *event)
{
    if (!activeWidget_ || !enabled_)
        return false;

    QMouseEvent *const mouseEvent = dynamic_cast<QMouseEvent *>(event);
    if (!mouseEvent)
        return false;

    // 鼠标松开时，清空 originPos_，防止被重复使用
    originPos_ = std::nullopt;

    // 获得当前可接受响应的区域
    const auto validRect = acceptedResponseRect();

    // 如果画布漫游行为不是"禁用橡皮筋"，结束画框，执行后续逻辑
    if (mouseEvent->button() == Qt::LeftButton) {
        if (!rubberBand_)
            return false;

        // 结束画框，隐藏橡皮筋
        rubberBand_->setVisible(false);

        // 画框尺寸小于有效范围，不执行流控
        if (rubberBand_->width() < kRubberBandValidLength &&
            rubberBand_->height() < kRubberBandValidLength)
            return false;

        // 橡皮筋矩形转为归一化数据
        double dX =
            (double)(rubberBand_->x() - validRect.x()) / validRect.width() * zoomRect_.width() +
            zoomRect_.x();
        double dY =
            (double)(rubberBand_->y() - validRect.y()) / validRect.height() * zoomRect_.height() +
            zoomRect_.y();
        double dW = (double)rubberBand_->width() / validRect.width() * zoomRect_.width();
        double dH = (double)rubberBand_->height() / validRect.height() * zoomRect_.height();

        // 执行放大
        setZoomRect(QRectF(dX, dY, dW, dH));

        // 重置橡皮筋大小
        rubberBand_->setGeometry(0, 0, 0, 0);
        return true;
    }

    return true;
}

bool WidgetDigitalZoomController::handleMouseDoubleClickEvent(QEvent *event)
{
    if (!activeWidget_ || !enabled_)
        return false;

    // 不做任何处理
    return false;
}

bool WidgetDigitalZoomController::handleWheelEvent(QEvent *event)
{
    if (!activeWidget_ || !enabled_)
        return false;

    QWheelEvent *const wheelEvent = dynamic_cast<QWheelEvent *>(event);
    if (!wheelEvent)
        return false;

    // 如果不在可接受响应的区域，就返回
    const auto validRect = acceptedResponseRect();
    if (!validRect.contains(wheelEvent->position().toPoint())) {
        return false;
    }

    // 执行控制
    const auto scale = wheelEvent->angleDelta().y() > 0 ? 1.0 / wheelFactor_ : wheelFactor_;
    setZoomScale(scale);

    return false;
}

bool WidgetDigitalZoomController::handleResizeEvent(QEvent *event)
{
    Q_UNUSED(event);
    return false;
}

void WidgetDigitalZoomController::initByActiveWidget(QWidget *activeWidget)
{
    rubberBand_.reset(new RubberBand(QRubberBand::Rectangle, activeWidget));
}

QRect WidgetDigitalZoomController::acceptedResponseRect() const
{
    if (!activeWidget_)
        return {};

    const auto validRect = activeWidget_->rect();
    return validRect.adjusted(
        std::max(0, logicContentMargins_.left()), std::max(0, logicContentMargins_.top()),
        -std::max(0, logicContentMargins_.right()), -std::max(0, logicContentMargins_.bottom()));
}

QPoint WidgetDigitalZoomController::constrainPointToRect(const QPoint &point,
                                                         const QRect &rect) const
{
    if (rect.contains(point))
        return point;

    int x = qBound(rect.left(), point.x(), rect.right());
    int y = qBound(rect.top(), point.y(), rect.bottom());
    return QPoint(x, y);
}