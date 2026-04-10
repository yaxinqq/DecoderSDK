#include "SimplePlayer.h"

#include "ui_SimplePlayer.h"

#include <QFileDialog>
#include <QMenu>
#include <QShortcut>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace {
QMargins calculateVideoContainerSpacerMargin(const QSize &containerSize, double videoRatio)
{
    if (std::fabs(videoRatio - 0) < 1e-12)
        return {0, 0, 0, 0};

    const auto containerRatio = (double)containerSize.width() / containerSize.height();

    if (containerRatio >= videoRatio && std::fabs(containerRatio - videoRatio) > 1e-12) {
        int nOffset = (containerSize.width() - containerSize.height() * videoRatio) / 2;
        nOffset = nOffset < 0 ? 0 : nOffset;
        return {nOffset, 0, nOffset, 0};
    } else {
        int nOffset = (containerSize.height() - containerSize.width() * (1 / videoRatio)) / 2;
        nOffset = nOffset < 0 ? 0 : nOffset;
        return {0, nOffset, 0, nOffset};
    }
}

float clampValue(const float value, const float minValue, const float maxValue)
{
    return std::max(minValue, std::min(value, maxValue));
}

float quantizeByStep(const float value, const float base, const float step, const float minValue,
                     const float maxValue)
{
    const float clamped = clampValue(value, minValue, maxValue);
    const float steps = std::round((clamped - base) / step);
    return clampValue(base + steps * step, minValue, maxValue);
}

float snapDefault(const float value, const float defaultValue, const float epsilon = 1.0e-5f)
{
    return std::abs(value - defaultValue) <= epsilon ? defaultValue : value;
}
} // namespace

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

void SimplePlayer::onTotalTimeRecved(int64_t totalTime)
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

void SimplePlayer::onVideoRectChanged(const QRect &rect)
{
    if (!digitalZoomCtrl_) {
        return;
    }

    const auto isValid = rect.isValid();
    digitalZoomCtrl_->setEnabled(isValid);
    if (isValid) {
        digitalZoomCtrl_->setActiveWidgetLogicContentMargins(calculateVideoContainerSpacerMargin(
            ui->player->size(), static_cast<double>(rect.width()) / rect.height()));
    }
}

void SimplePlayer::onDigitalZoomRectChanged(const QRectF &rect)
{
    ui->player->setDigitalZoomRect(rect);
}

void SimplePlayer::contextMenuEvent(QContextMenuEvent *event)
{
    return QWidget::contextMenuEvent(event);

    // 和电子放大冲突，暂不处理
    QMenu menu(this);
    menu.addAction(QStringLiteral("亮度 +10%"), this, [this]() { adjustBrightness(0.1f); });
    menu.addAction(QStringLiteral("亮度 -10%"), this, [this]() { adjustBrightness(-0.1f); });
    menu.addAction(QStringLiteral("对比度 +10%"), this, [this]() { adjustContrast(0.1f); });
    menu.addAction(QStringLiteral("对比度 -10%"), this, [this]() { adjustContrast(-0.1f); });
    menu.addAction(QStringLiteral("饱和度 +10%"), this, [this]() { adjustSaturation(0.1f); });
    menu.addAction(QStringLiteral("饱和度 -10%"), this, [this]() { adjustSaturation(-0.1f); });
    menu.addAction(QStringLiteral("色调 +10%"), this, [this]() { adjustHue(0.1f); });
    menu.addAction(QStringLiteral("色调 -10%"), this, [this]() { adjustHue(-0.1f); });
    menu.exec(event->globalPos());
}

void SimplePlayer::initUi()
{
    ui->urlEdit->setText(QStringLiteral("D:/WorkSpace/test_video/test.mp4"));

    // 初始化滑块
    ui->ptsSlider->setMinimum(0);
    ui->ptsSlider->setValue(0);

    digitalZoomCtrl_ = new WidgetDigitalZoomController(ui->player);
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
    connect(ui->player, &RtspStreamPlayer::videoRectChanged, this,
            &SimplePlayer::onVideoRectChanged);

    connect(digitalZoomCtrl_, &WidgetDigitalZoomController::zoomRectChanged, this,
            &SimplePlayer::onDigitalZoomRectChanged);

    // 连接滑块信号
    connect(ui->ptsSlider, &QSlider::valueChanged, this, &SimplePlayer::onSliderValueChanged);
    connect(ui->ptsSlider, &QSlider::sliderPressed, this, &SimplePlayer::onSliderPressed);
    connect(ui->ptsSlider, &QSlider::sliderReleased, this, &SimplePlayer::onSliderReleased);

    auto *resetShortcut = new QShortcut(QKeySequence(Qt::Key_Q), this);
    connect(resetShortcut, &QShortcut::activated, this, &SimplePlayer::resetColorAdjustments);

    auto *incBrightnessShortcut = new QShortcut(QKeySequence(Qt::Key_E), this);
    connect(incBrightnessShortcut, &QShortcut::activated, this,
            [this]() { adjustBrightness(0.01f); });
    auto *decBrightnessShortcut = new QShortcut(QKeySequence(Qt::Key_W), this);
    connect(decBrightnessShortcut, &QShortcut::activated, this,
            [this]() { adjustBrightness(-0.01f); });

    auto *incContrastShortcut = new QShortcut(QKeySequence(Qt::Key_T), this);
    connect(incContrastShortcut, &QShortcut::activated, this, [this]() { adjustContrast(0.01f); });
    auto *decContrastShortcut = new QShortcut(QKeySequence(Qt::Key_R), this);
    connect(decContrastShortcut, &QShortcut::activated, this, [this]() { adjustContrast(-0.01f); });

    auto *incSaturationShortcut = new QShortcut(QKeySequence(Qt::Key_U), this);
    connect(incSaturationShortcut, &QShortcut::activated, this,
            [this]() { adjustSaturation(0.01f); });
    auto *decSaturationShortcut = new QShortcut(QKeySequence(Qt::Key_Y), this);
    connect(decSaturationShortcut, &QShortcut::activated, this,
            [this]() { adjustSaturation(-0.01f); });

    auto *incHueShortcut = new QShortcut(QKeySequence(Qt::Key_O), this);
    connect(incHueShortcut, &QShortcut::activated, this, [this]() { adjustHue(0.01f); });
    auto *decHueShortcut = new QShortcut(QKeySequence(Qt::Key_I), this);
    connect(decHueShortcut, &QShortcut::activated, this, [this]() { adjustHue(-0.01f); });
}

void SimplePlayer::resetColorAdjustments()
{
    brightness_ = 0.0f;
    contrast_ = 1.0f;
    saturation_ = 1.0f;
    hue_ = 0.0f;
    applyColorAdjustments();
}

void SimplePlayer::adjustBrightness(float percent)
{
    const float range = 1.0f - (-1.0f);
    const float step = range * percent;
    const float quantum = range * std::abs(percent);
    brightness_ = quantizeByStep(brightness_ + step, 0.0f, quantum, -1.0f, 1.0f);
    brightness_ = snapDefault(brightness_, 0.0f);
    applyColorAdjustments();
}

void SimplePlayer::adjustContrast(float percent)
{
    const float range = 3.0f - 0.0f;
    const float step = range * percent;
    const float quantum = range * std::abs(percent);
    contrast_ = quantizeByStep(contrast_ + step, 1.0f, quantum, 0.0f, 3.0f);
    contrast_ = snapDefault(contrast_, 1.0f);
    applyColorAdjustments();
}

void SimplePlayer::adjustSaturation(float percent)
{
    const float range = 2.0f - 0.0f;
    const float step = range * percent;
    const float quantum = range * std::abs(percent);
    saturation_ = quantizeByStep(saturation_ + step, 1.0f, quantum, 0.0f, 2.0f);
    saturation_ = snapDefault(saturation_, 1.0f);
    applyColorAdjustments();
}

void SimplePlayer::adjustHue(float percent)
{
    const float range = 0.5f - (-0.5f);
    const float step = range * percent;
    const float quantum = range * std::abs(percent);
    hue_ = quantizeByStep(hue_ + step, 0.0f, quantum, -0.5f, 0.5f);
    hue_ = snapDefault(hue_, 0.0f);
    applyColorAdjustments();
}

void SimplePlayer::applyColorAdjustments()
{
    ui->player->setColorAdjustments(brightness_, contrast_, saturation_, hue_);
}
