#pragma once

#include <QGraphicsRectItem>
#include <QRubberBand>

#pragma region RubberBand
/*!
 * \class RubberBand
 *
 * \brief 重绘橡皮筋控件
 *
 * \author W.C.
 * \date 2024/1/21
 */
class RubberBand : public QRubberBand
{
public:
    explicit RubberBand(Shape s, QWidget *parent = nullptr);
    ~RubberBand();

protected:
    void paintEvent(QPaintEvent *event) override;
};
#pragma endregion


#pragma region RubberBandItem
/*!
 * \class RubberBandItem
 *
 * \brief 用于QGraphicsView的橡皮筋控件
 *
 * \author ZYX
 * \date 2024/1/21
 */
class RubberBandItem : public QGraphicsRectItem
{
public:
    using QGraphicsRectItem::QGraphicsRectItem;

protected:
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget = nullptr) override;
};
#pragma endregion
