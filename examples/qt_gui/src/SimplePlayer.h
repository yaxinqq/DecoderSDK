#pragma once
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

    void onTotalTimeRecved(int totalTime);
    void onPtsChanged(double pts);
    void onSliderValueChanged(int value);
    void onSliderPressed();
    void onSliderReleased();
    void onSpeedBtnClicked();

private:
    void initUi();
    void initConnection();

private:
    Ui::SimplePlayer *ui = nullptr;
    bool isSliderPressed_ = false; // 标记滑块是否被按下
    int totalTime_ = 0;            // 总时长
};
