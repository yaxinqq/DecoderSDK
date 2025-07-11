#include "SimplePlayer.h"

#include "ui_SimplePlayer.h"

#include <QUuid>

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

void SimplePlayer::initUi()
{
    ui->urlEdit->setText(QStringLiteral("C:/Users/zhkj/Desktop/test_video/test.mp4"));
}

void SimplePlayer::initConnection()
{
    connect(ui->startBtn, &QPushButton::clicked, this, &SimplePlayer::onStartBtnClicked);
    connect(ui->stopBtn, &QPushButton::clicked, this, &SimplePlayer::onStopBtnClicked);
    connect(ui->pauseBtn, &QPushButton::clicked, this, &SimplePlayer::onPauseBtnClicked);
    connect(ui->resumeBtn, &QPushButton::clicked, this, &SimplePlayer::onResumeBtnClicked);
}
