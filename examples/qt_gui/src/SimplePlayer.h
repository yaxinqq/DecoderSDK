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

private:
    void initUi();
    void initConnection();

private:
    Ui::SimplePlayer *ui = nullptr;
};
