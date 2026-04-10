#pragma once
#include "WidgetDigitalZoomController.h"

#include <QContextMenuEvent>
#include <QWidget>

namespace Ui {
class SimplePlayer;
}

class SimplePlayer : public QWidget {
    Q_OBJECT

public:
    explicit SimplePlayer(QWidget *parent = nullptr);
    ~SimplePlayer();

private slots:
    void onStartBtnClicked();
    void onStopBtnClicked();
    void onPauseBtnClicked();
    void onResumeBtnClicked();
    void onStartRecordBtnClicked();
    void onStopRecordBtnClicked();

    void onTotalTimeRecved(int64_t totalTime);
    void onPtsChanged(double pts);
    void onSliderValueChanged(int value);
    void onSliderPressed();
    void onSliderReleased();
    void onSpeedBtnClicked();

    void onVideoRectChanged(const QRect &rect);
    void onDigitalZoomRectChanged(const QRectF &rect);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void initUi();
    void initConnection();
    void resetColorAdjustments();
    void adjustBrightness(float percent);
    void adjustContrast(float percent);
    void adjustSaturation(float percent);
    void adjustHue(float percent);
    void applyColorAdjustments();

private:
    Ui::SimplePlayer *ui = nullptr;
    bool isSliderPressed_ = false; // 标记滑块是否被按下
    int64_t totalTime_ = 0;        // 总时长

    WidgetDigitalZoomController *digitalZoomCtrl_ = nullptr;

    // 颜色调整参数
    float brightness_ = 0.0f;
    float contrast_ = 1.0f;
    float saturation_ = 1.0f;
    float hue_ = 0.0f;
};
