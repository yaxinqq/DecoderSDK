#include "RubberBand.h"

#include "../Base/InternalUtils.h"

#include <QPainter>

namespace {
    const QColor kColor = Qt::red;
    const int kPenWidth = 4;
    const double kBrushAlpha = 0.05;
}

#pragma region RubberBand
RubberBand::RubberBand(Shape s, QWidget *parent)
    : QRubberBand(s, parent)
{
    setAttribute(Qt::WA_TranslucentBackground);
}

RubberBand::~RubberBand()
{
}

void RubberBand::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

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


#pragma region RubberBandItem
namespace {
    constexpr int kRectMinWidth = 1;
    constexpr int kRectMinHeight = 1;
}

void RubberBandItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
    Q_UNUSED(option);
    Q_UNUSED(widget);

    painter->save();

    // 将水平缩放和竖直缩放的数值在线宽上归一 m11 水平缩放，m22 竖直缩放
    const auto newPenWidth = kPenWidth / qMax(painter->transform().m11(), painter->transform().m22());

    painter->setPen(QPen(kColor, newPenWidth));

    QColor brushColor = kColor;
    brushColor.setAlphaF(kBrushAlpha);
    painter->setBrush(brushColor);

    const auto rect = boundingRect();
    if (utils::greaterAndEqual(rect.width(), kRectMinWidth)
        && utils::greaterAndEqual(rect.height(), kRectMinHeight)
        && !rect.isNull()
        && rect.isValid()) {
        painter->drawRect(boundingRect());
    }

    painter->restore();
}
#pragma endregion
