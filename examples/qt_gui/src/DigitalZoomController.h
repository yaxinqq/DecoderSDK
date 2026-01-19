#pragma once

#include <QEvent>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QRectF>

#include <optional>

class DigitalZoomController : public QObject {
    Q_OBJECT

public:
    explicit DigitalZoomController(QObject *parent = nullptr);
    virtual ~DigitalZoomController();

    /*
     * @brief 设置活跃窗口
     *
     * @param widget 活跃窗口，不能为空
     */
    virtual void setActicedWidget(QWidget *widget) = 0;

    /*
     * @brief 获取当前矩形区域
     *
     * @return 矩形区域 (归一化)
     */
    QRectF zoomRect() const;
    /**
     * @brief 设置当前矩形区域，会在设置时，进行有效性转换和宽高比转换
     *
     * @param curRect 矩形区域 (归一化)
     */
    void setZoomRect(QRectF rect);
    /**
     * @brief 设置缩放系数
     *
     * @param scale 缩放系数
     */
    void setZoomScale(double scale);
    /**
     * @brief zoomRect移动，传入的参数是界面实际移动坐标，非归一值，会在此函数中进行转换
     *
     * @param xStep 横轴移动像素值
     * @param yStep 纵轴移动像素值
     */
    void zoomRectMove(int xStep, int yStep);
    /*
     * @brief 重置当前矩形区域
     */
    void resetZoomRect();

    /**
     * @brief 设置是否启用
     *
     * @param enabled 是否启用
     */
    void setEnabled(bool enabled);
    /**
     * @brief 当前是否启用
     *
     * @return 当前是否启用
     */
    bool isEnabled() const;

    /**
     * @brief 获得当前最大电子放大倍率
     *
     * @return 当前最大电子放大倍率
     */
    int maxDigitalZoomFactor() const;
    /**
     * @brief 设置当前最大电子放大倍率
     *
     * @param maxFactor 最大倍率
     */
    void setMaxDigitalZoomFactor(int maxFactor);

signals:
    /**
     * @brief 电子放大区域发生变化
     *
     * @param rect 放大区域
     */
    void zoomRectChanged(const QRectF &rect);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

protected:
    /*
     * @brief 处理鼠标的按下事件
     *
     * @param event 事件循环传递过来的鼠标事件
     * @return 是否需要阻断事件的传递
     */
    virtual bool handleMousePressEvent(QEvent *event) = 0;
    /*
     * @brief 处理鼠标的移动事件
     *
     * @param event 事件循环传递过来的鼠标事件
     * @return 是否需要阻断事件的传递
     */
    virtual bool handleMouseMoveEvent(QEvent *event) = 0;
    /*
     * @brief 处理鼠标的松开事件
     *
     * @param event 事件循环传递过来的鼠标事件
     * @return 是否需要阻断事件的传递
     */
    virtual bool handleMouseReleaseEvent(QEvent *event) = 0;
    /*
     * @brief 处理鼠标的双击事件
     *
     * @param event 事件循环传递过来的鼠标事件
     * @return 是否需要阻断事件的传递
     */
    virtual bool handleMouseDoubleClickEvent(QEvent *event) = 0;
    /*
     * @brief 处理鼠标的滚轮事件
     *
     * @param event 事件循环传递过来的鼠标事件
     * @return 是否需要阻断事件的传递
     */
    virtual bool handleWheelEvent(QEvent *event) = 0;
    /*
     * @brief 处理窗口大小变化的事件
     *
     * @param event 事件循环传递过来的窗口大小变化事件
     * @return 是否需要阻断事件的传递
     */
    virtual bool handleResizeEvent(QEvent *event) = 0;

    /*
     * @brief 获得接受响应的矩形区域，根据窗口本身的rect和logicContentMargins计算而出
     *
     * @return 接受响应的矩形区域，默认是activeWidget_的rect
     */
    virtual QRect acceptedResponseRect() const;

    /*
     * @brief 将坐标点约束在QRect内，如果超出，则找到距离QRect最近的点
     *
     * @param point 待约束的点
     * @param rect 约束的矩形区域
     * @return 接受响应的矩形区域
     */
    QPoint constrainPointToRect(const QPoint &point, const QRect &rect) const;

protected:
    // 鼠标左键按下
    bool leftBtnPressed_ = false;
    // 鼠标左键按下位置
    QPoint leftBtnPressedPos_;
    // 鼠标右键按下
    bool rightBtnPressed_ = false;
    // 鼠标右键按下位置
    QPoint rightBtnPressedPos_;
    // 画框基准位置
    std::optional<QPointF> originPos_ = std::nullopt;
    // 缩放系数
    double wheelFactor_ = 0.8;

    // 最大放大倍率
    int maxDigitalZoomFactor_ = 40;

    // 电子放大的矩形区域
    QRectF zoomRect_ = QRectF(0, 0, 1, 1);
    // 电子放大-移动时，右键按下时的坐标
    QPoint pressedPos_;

    // 当前活跃窗口的指针
    QPointer<QWidget> activeWidget_;

    // 当前是否启用
    bool enabled_ = true;

    // 窗口类型和事件类型处理函数的对应关系
    QMap<QEvent::Type, bool (DigitalZoomController::*)(QEvent *)> funcs_ = {
        {QEvent::MouseButtonPress, &DigitalZoomController::handleMousePressEvent},
        {QEvent::MouseMove, &DigitalZoomController::handleMouseMoveEvent},
        {QEvent::MouseButtonRelease, &DigitalZoomController::handleMouseReleaseEvent},
        {QEvent::MouseButtonDblClick, &DigitalZoomController::handleMouseDoubleClickEvent},
        {QEvent::Wheel, &DigitalZoomController::handleWheelEvent},
        {QEvent::Resize, &DigitalZoomController::handleResizeEvent},
    };
};
