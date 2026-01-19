#pragma once

#include "DigitalZoomController.h"

#include <QEvent>
#include <QScopedPointer>
#include <QWidget>

class RubberBand;

/*!
 * \class WidgetDigitalZoomController
 *
 * \brief 显示介质是QWidget时的画布漫游功能
 *
 * \author zyx
 * \date 2025/3/31
 */
class WidgetDigitalZoomController : public DigitalZoomController {
    Q_OBJECT

public:
    explicit WidgetDigitalZoomController(QWidget *parent = nullptr);
    virtual ~WidgetDigitalZoomController();

    /*
     * @brief
     * 设置活跃窗口，但是不会改变当前生效的deviceId和channelId，如果需要变更画布漫游生效的设备，建议调用uninitialize和initialize重新初始化
     *
     * @param widget 活跃窗口，不能为空
     */
    void setActicedWidget(QWidget *widget) override;

    /*
     * @brief
     * 设置活动窗口的逻辑边距，画布漫游控制器只在边距内的窗体范围中才响应。该接口主要用于固定视窗等涉及到任意改变尺寸的窗体，
     *        为了保持融合画面的宽高比，就必须不停的设置layout()->setContentsMargins，会导致画面闪烁，所以改为在画布漫游控制器中去实现过滤。
     *        但是当前仅对QWidget生效。QGraphicsView还需要考虑是否同步修改QGraphicsScene的大小，不然标签那些Item的位置不对。
     *        后续有需求了，在考虑对QGraphicsView进行支持
     *
     * @param margins 活动窗口的逻辑边距，是通过videoRatio和活动窗口的宽高计算出来的
     */
    void setActiveWidgetLogicContentMargins(const QMargins &margins);

    /*
     * @brief 获得活动窗口的逻辑边距
     *
     * @return 活动窗口的逻辑边距
     */
    QMargins activeWidgetContentMargins() const;

protected:
    /*
     * @brief 处理鼠标的按下事件
     *
     * @param event 事件循环传递过来的鼠标事件
     * @return 是否需要阻断事件的传递
     */
    bool handleMousePressEvent(QEvent *event) override;
    /*
     * @brief 处理鼠标的移动事件
     *
     * @param event 事件循环传递过来的鼠标事件
     * @return 是否需要阻断事件的传递
     */
    bool handleMouseMoveEvent(QEvent *event) override;
    /*
     * @brief 处理鼠标的松开事件
     *
     * @param event 事件循环传递过来的鼠标事件
     * @return 是否需要阻断事件的传递
     */
    bool handleMouseReleaseEvent(QEvent *event) override;
    /*
     * @brief 处理鼠标的双击事件
     *
     * @param event 事件循环传递过来的鼠标事件
     * @return 是否需要阻断事件的传递
     */
    bool handleMouseDoubleClickEvent(QEvent *event) override;
    /*
     * @brief 处理鼠标的滚轮事件
     *
     * @param event 事件循环传递过来的鼠标事件
     * @return 是否需要阻断事件的传递
     */
    bool handleWheelEvent(QEvent *event) override;
    /*
     * @brief 处理窗口大小变化的事件
     *
     * @param event 事件循环传递过来的窗口大小变化事件
     * @return 是否需要阻断事件的传递
     */
    bool handleResizeEvent(QEvent *event) override;

    /*
     * @brief 传入的活动窗口为QWidget时的初始化函数
     *
     * @param activeWidget 活动窗口
     */
    void initByActiveWidget(QWidget *activeWidget);

    /*
     * @brief 获得接受响应的矩形区域，根据窗口本身的rect和logicContentMargins计算而出
     *
     * @return 接受响应的矩形区域
     */
    QRect acceptedResponseRect() const override;

    /*
     * @brief 将坐标点约束在QRect内，如果超出，则找到距离QRect最近的点
     *
     * @param point 待约束的点
     * @param rect 约束的矩形区域
     * @return 接受响应的矩形区域
     */
    QPoint constrainPointToRect(const QPoint &point, const QRect &rect) const;

private:
    // 当前活跃窗口的逻辑边距
    QMargins logicContentMargins_ = {0, 0, 0, 0};

    // 画框橡皮筋，生命周期和StreamCtrl一致，父对象为activeWidget，QScopedPointer的析构会调用rubberBand_的deleteLater，不用担心指针重复释放的问题
    QScopedPointer<RubberBand, QScopedPointerDeleteLater> rubberBand_;
};
