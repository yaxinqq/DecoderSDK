#include "SimplePlayer.h"

#include "ui_SimplePlayer.h"

#include <QFileDialog>
#include <QUuid>

#include <cmath>

SimplePlayer::SimplePlayer(QWidget *parent) : QWidget(parent), ui{new Ui::SimplePlayer}
{
    ui->setupUi(this);

    initUi();
    initConnection();
}

SimplePlayer::~SimplePlayer()
{
    delete ui;
    ui = nullptr;
}

void SimplePlayer::onStartBtnClicked()
{
    const auto url = ui->urlEdit->text();
    ui->player->open(url, QUuid::createUuid().toString(QUuid::StringFormat::WithoutBraces), {});
}

void SimplePlayer::onStopBtnClicked()
{
    ui->player->close();
}

void SimplePlayer::onPauseBtnClicked()
{
    ui->player->pause();
}

void SimplePlayer::onResumeBtnClicked()
{
    ui->player->resume();
}

void SimplePlayer::onStartRecordBtnClicked()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存文件"), QStringLiteral(""), QStringLiteral("* (*.*)"));

    if (path.isEmpty()) {
        return;
    }

    ui->player->startRecoding(path);
}

void SimplePlayer::onStopRecordBtnClicked()
{
    ui->player->stopRecoding();
}

void SimplePlayer::onTotalTimeRecved(int totalTime)
{
    totalTime_ = totalTime;
    ui->endTimeLabel->setText(QString::number(totalTime));
    ui->ptsSlider->setMaximum(totalTime);
}

void SimplePlayer::onPtsChanged(double pts)
{
    ui->startTimeLabel->setText(QString::number(std::round(pts)));

    // 只有在滑块没有被按下时才更新滑块位置，避免拖拽时的冲突
    if (!isSliderPressed_) {
        ui->ptsSlider->setValue(static_cast<int>(std::round(pts)));
    }
}

void SimplePlayer::onSliderValueChanged(int value)
{
    if (isSliderPressed_) {
        ui->startTimeLabel->setText(QString::number(value)); 
    }
}

void SimplePlayer::onSliderPressed()
{
    isSliderPressed_ = true;
}

void SimplePlayer::onSliderReleased()
{
    isSliderPressed_ = false;
    // 只在松开时执行seek
    ui->player->seek(static_cast<double>(ui->ptsSlider->value()));
}

void SimplePlayer::onSpeedBtnClicked()
{
    ui->player->setSpeed(ui->speedEdit->text().toDouble());
}

void SimplePlayer::initUi()
{
    ui->urlEdit->setText(QStringLiteral("D:/WorkSpace/test_video/test.mp4"));

    // 初始化滑块
    ui->ptsSlider->setMinimum(0);
    ui->ptsSlider->setValue(0);
}

void SimplePlayer::initConnection()
{
    connect(ui->startBtn, &QPushButton::clicked, this, &SimplePlayer::onStartBtnClicked);
    connect(ui->stopBtn, &QPushButton::clicked, this, &SimplePlayer::onStopBtnClicked);
    connect(ui->pauseBtn, &QPushButton::clicked, this, &SimplePlayer::onPauseBtnClicked);
    connect(ui->resumeBtn, &QPushButton::clicked, this, &SimplePlayer::onResumeBtnClicked);
    connect(ui->startRecordBtn, &QPushButton::clicked, this,
            &SimplePlayer::onStartRecordBtnClicked);
    connect(ui->stopRecordBtn, &QPushButton::clicked, this, &SimplePlayer::onStopRecordBtnClicked);
    connect(ui->speedBtn, &QPushButton::clicked, this, &SimplePlayer::onSpeedBtnClicked);

    connect(ui->player, &RtspStreamPlayer::totalTimeRecved, this, &SimplePlayer::onTotalTimeRecved);
    connect(ui->player, &RtspStreamPlayer::ptsChanged, this, &SimplePlayer::onPtsChanged);

    // 连接滑块信号
    connect(ui->ptsSlider, &QSlider::valueChanged, this, &SimplePlayer::onSliderValueChanged);
    connect(ui->ptsSlider, &QSlider::sliderPressed, this, &SimplePlayer::onSliderPressed);
    connect(ui->ptsSlider, &QSlider::sliderReleased, this, &SimplePlayer::onSliderReleased);
}
