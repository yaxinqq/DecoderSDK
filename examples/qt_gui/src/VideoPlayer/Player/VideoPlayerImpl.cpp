#include "VideoPlayerImpl.h"
#include "DisplayRenderer.h"
#include "InternalUtils.h"
#include "Stream.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace {
    float clampBrightness(const float value)
    {
        return std::clamp(value, -1.0f, 1.0f);
    }

    float clampContrast(const float value)
    {
        return std::clamp(value, 0.0f, 3.0f);
    }

    float clampSaturation(const float value)
    {
        return std::clamp(value, 0.0f, 2.0f);
    }

    float clampHue(const float value)
    {
        return std::clamp(value, -1.0f, 1.0f);
    }

    // 根据给定的宽高，得到对应的宽高比字符串 1:xxx 或是 xxx:1
    QString getAspectRatioString(int width, int height)
    {
        if (width < height) {
            return QStringLiteral("1:%1").arg(QString::number(height / (double)width, 'f', 2));
        } else {
            return QStringLiteral("%1:1").arg(QString::number(width / (double)height, 'f', 2));
        }
    }
} // namespace

VideoPlayerImpl::VideoPlayerImpl(QObject *parent)
    : QObject(parent)
    , playerState_{ Stream::PlayerState::Stop }
    , strText_{}
    , uuid_{ QUuid::createUuid().toString(QUuid::StringFormat::WithoutBraces) }
{
    QTimer *timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, [this]() {
        fps_ = frameCount_;
        frameCount_ = 0;
        emit aboutToUpdate();
    });
    timer->start();

    painterFont_ = qGuiApp->font();
    painterFont_.setPixelSize(30);
    painterFont_.setWeight(QFont::Bold);
    QFontMetrics fm(painterFont_);
    fpsTextWidth_ = fm.horizontalAdvance(QString("FPS: 99")) + 10;
    fpsTextHeight_ = fm.height();

    fpsVisible_ = true;
    //================

    connect(this, &VideoPlayerImpl::playerStateChanged, this, &VideoPlayerImpl::onPlayerStateChanged);
}

VideoPlayerImpl::~VideoPlayerImpl()
{
    // 通知外部，本类即将被删除
    emit aboutToDestroyed(uuid_);
}

QString VideoPlayerImpl::id() const
{
    return uuid_;
}

void VideoPlayerImpl::initialize(QOpenGLContext *context, QSurface *surface)
{
}

void VideoPlayerImpl::paintGL(const QRect &widgetRect, const QRect &needRenderedRect, const QPoint &referencePt)
{
    // widgetRect_ = widgetRect;
    QOpenGLContext *curContext = QOpenGLContext::currentContext();
    if (!curContext)
        return;

    // 黑色背景
    clear(needRenderedRect, referencePt);

    if (render_.expired())
        return;

    if (playerState_ == Stream::PlayerState::Playing || playerState_ == Stream::PlayerState::Pause) {
        if (playerState_ == Stream::PlayerState::Playing)
            frameCount_++;

        QRect videoRect = calculateVideoRect(needRenderedRect);
        // 调整视频绘制区域
        const auto diff = videoRect.bottomLeft() - referencePt;
        curContext->functions()->glViewport(diff.x(), -diff.y(), videoRect.width(), videoRect.height());

        // 调整到自身坐标系
        videoRect = QRect(videoRect.topLeft() - needRenderedRect.topLeft(), videoRect.size());
        if (videoRect != videoRect_) {
            // 在实际赋值时，videoRect应该保持自身坐标系
            videoRect_ = videoRect;
            emit videoRectChanged(videoRect_);
            resetStatisticalInfoUpdateTimer();
        }

        // 渲染帧
        if (auto sharedRender = render_.lock(); sharedRender) {
            sharedRender->draw(processParam_);
        }
    }
}

void VideoPlayerImpl::resize(int w, int h)
{
    const QRect widgetRect = QRect(0, 0, w, h);
    if (widgetRect != widgetRect_) {
        widgetRect_ = widgetRect;
        emit widgetRectChanged(widgetRect_);

        const QRect videoRect = calculateVideoRect(widgetRect_);
        if (videoRect != videoRect_) {
            videoRect_ = videoRect;
            emit videoRectChanged(videoRect_);
        }

        resetStatisticalInfoUpdateTimer();
    }
}

void VideoPlayerImpl::clear(const QRect &needRenderedRect, const QPoint &referencePt)
{
    QOpenGLContext *curContext = QOpenGLContext::currentContext();
    if (!curContext)
        return;

    // 清屏，绘制黑色背景
    auto *const f = curContext->functions();

    const auto diff = needRenderedRect.bottomLeft() - referencePt;
    f->glViewport(diff.x(), -diff.y(), needRenderedRect.width(), needRenderedRect.height());

    // 启用裁剪
    f->glEnable(GL_SCISSOR_TEST);
    f->glScissor(diff.x(), -diff.y(), needRenderedRect.width(), needRenderedRect.height());

    // 设置清除颜色为黑色并清除
    f->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT);

    // 禁用裁剪
    f->glDisable(GL_SCISSOR_TEST);
}

void VideoPlayerImpl::clearByPainter(QPainter *painter, const QRect &rect)
{
    painter->fillRect(rect, Qt::black);
}

Stream::AspectRatioMode VideoPlayerImpl::aspectRatioMode() const
{
    return aspectRatio_;
}

void VideoPlayerImpl::setAspectRatioMode(Stream::AspectRatioMode ratio)
{
    aspectRatio_ = ratio;
}

void VideoPlayerImpl::paintCommon(QPainter *painter, const QRect &widgetRect)
{
    // widgetRect_ = widgetRect;

    painter->save();
    painter->setFont(painterFont_);

    painter->setPen(Qt::white);
    painter->drawText(widgetRect_, Qt::AlignCenter, strText_);

    if (fpsVisible_) {
        painter->setPen(QColor(255, 242, 0));

        //// 绘制统计信息
        //painter->fillRect(widgetRect, QColor(0, 0, 0, 127));

        //QTextOption textOpt;
        //textOpt.setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        //textOpt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        //painter->drawText(widgetRect.adjusted(30, 0, 0, 0), statisticalInfo_, textOpt);

        // 绘制FPS
        painter->drawText(widgetRect_.topRight() - QPoint(fpsTextWidth_, -fpsTextHeight_), QString("FPS: %1").arg(fps_));
    }

    painter->restore();
}

void VideoPlayerImpl::renderToImage(const QSize &size, QImage &image)
{
    auto sharedRender = render_.lock();
    if (!sharedRender)
        return;

    sharedRender->currentFrameToImage(processParam_, size, image);
}

void VideoPlayerImpl::renderToImage(QImage &image)
{
    renderToImage(QSize(frameWidth_, frameHeight_), image);
}

void VideoPlayerImpl::setPlayerState(Stream::PlayerState state)
{
    playerState_ = state;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    emit playerStateChanged(playerState_);
#endif

    if (playerState_ == Stream::PlayerState::Playing) {
    } else if (playerState_ == Stream::PlayerState::Pause) {
    } else if (playerState_ == Stream::PlayerState::Stop) {
    }

    emit aboutToUpdate();
}

void VideoPlayerImpl::setDigitalZoomRect(const QRectF &rect)
{
    processParam_.digitalZoomRect = rect;
    emit aboutToUpdate();
}

QRectF VideoPlayerImpl::digitalZoomRect() const
{
    return processParam_.digitalZoomRect;
}

void VideoPlayerImpl::setHorizontalFlip(bool flip)
{
    processParam_.horizontalFlip = flip;
    emit aboutToUpdate();
}

bool VideoPlayerImpl::isHorizontalFlip() const
{
    return processParam_.horizontalFlip;
}

void VideoPlayerImpl::setVecticalFlip(bool flip)
{
    processParam_.vecticalFlip = flip;
    emit aboutToUpdate();
}

bool VideoPlayerImpl::isVecticalFlip() const
{
    return processParam_.vecticalFlip;
}

void VideoPlayerImpl::setHorizontalAndVecticalFlip(bool hflip, bool vflip)
{
    processParam_.horizontalFlip = hflip;
    processParam_.vecticalFlip = vflip;
    emit aboutToUpdate();
}

QPair<bool, bool> VideoPlayerImpl::isHorizontalAndVecticalFlip() const
{
    return { processParam_.horizontalFlip, processParam_.vecticalFlip };
}

void VideoPlayerImpl::setColorAdjustments(float brightness, float contrast, float saturation, float hue)
{
    processParam_.brightness = clampBrightness(brightness);
    processParam_.contrast = clampContrast(contrast);
    processParam_.saturation = clampSaturation(saturation);
    processParam_.hue = clampHue(hue);
    emit aboutToUpdate();
}

void VideoPlayerImpl::setBrightness(float brightness)
{
    processParam_.brightness = clampBrightness(brightness);
    emit aboutToUpdate();
}

float VideoPlayerImpl::brightness() const
{
    return processParam_.brightness;
}

void VideoPlayerImpl::setContrast(float contrast)
{
    processParam_.contrast = clampContrast(contrast);
    emit aboutToUpdate();
}

float VideoPlayerImpl::contrast() const
{
    return processParam_.contrast;
}

void VideoPlayerImpl::setSaturation(float saturation)
{
    processParam_.saturation = clampSaturation(saturation);
    emit aboutToUpdate();
}

float VideoPlayerImpl::saturation() const
{
    return processParam_.saturation;
}

void VideoPlayerImpl::setHue(float hue)
{
    processParam_.hue = clampHue(hue);
    emit aboutToUpdate();
}

float VideoPlayerImpl::hue() const
{
    return processParam_.hue;
}

void VideoPlayerImpl::onDecoderEventChanged(const QString &url, decoder_sdk::EventType type,
                                            const std::shared_ptr<decoder_sdk::EventArgs> &event)
{
    switch (type) {
        case decoder_sdk::EventType::kStreamOpenFailed:
            strText_ = tr("流地址无效");
            emit errorOccured(Stream::ErrorType::kOpenError);
            break;
        case decoder_sdk::EventType::kStreamOpened: {
            if (auto *const streamEvent = dynamic_cast<decoder_sdk::StreamEventArgs *>(event.get());
                streamEvent && streamEvent->streamInfo.has_value()) {
                streamInfo_ = streamEvent->streamInfo;
                resetStatisticalInfoUpdateTimer();

                emit totalTimeRecved(streamInfo_->totalTime.value_or(-1));
            }
            break;
        }
        case decoder_sdk::EventType::kStreamLoopEnded:
            emit fileStreamLoopEnded();
            break;
        case decoder_sdk::EventType::kStreamReadError:
            emit errorOccured(Stream::ErrorType::kReadError);
            break;
        case decoder_sdk::EventType::kDecodeFirstFrame: {
            // 接收解码器信息，并按照MediaType分类
            if (auto *const decoderEvent = dynamic_cast<decoder_sdk::DecoderEventArgs *>(event.get());
                decoderEvent && decoderEvent->decoderInfo.has_value()) {
                decoderInfos_.insert(decoderEvent->mediaType, decoderEvent->decoderInfo);
                resetStatisticalInfoUpdateTimer();
            }
            break;
        }
        case decoder_sdk::EventType::kCreateDecoderFailed:
            strText_ = tr("资源不足");
            qWarning() << QStringLiteral("播放失败，解码器资源不足。");

            emit errorOccured(Stream::ErrorType::kDecodeError);
            break;
        case decoder_sdk::EventType::kRecordingStarted:
            emit recordStarted(utils::getFilePathFromRecordEvent(event));
            break;
        case decoder_sdk::EventType::kRecordingStopped:
            emit recordStopped(utils::getFilePathFromRecordEvent(event));
            break;
        case decoder_sdk::EventType::kRecordingError:
            emit errorOccured(Stream::ErrorType::kRecordError);
            break;
        case decoder_sdk::EventType::kSeekStarted:
            emit seekStarted();
            break;
        case decoder_sdk::EventType::kSeekSuccess:
            emit seekEnded(true);
            break;
        case decoder_sdk::EventType::kSeekFailed:
            emit seekEnded(false);
            break;
        default:
            break;
    }

    qDebug() << QStringLiteral("%1 occurred event: %2").arg(url, QString::fromStdString(event->description));
}

void VideoPlayerImpl::onStreamInfoUpdated(const std::optional<decoder_sdk::StreamInfo> &info)
{
    streamInfo_ = info;
    resetStatisticalInfoUpdateTimer();
}

void VideoPlayerImpl::onDecoderInfoUpdated(decoder_sdk::MediaType mediaType, const std::optional<decoder_sdk::DecoderInfo> &info)
{
    decoderInfos_.insert(mediaType, info);
    resetStatisticalInfoUpdateTimer();
}

void VideoPlayerImpl::onRendererNameChanged(const QString &name)
{
    rendererName_ = name;
    resetStatisticalInfoUpdateTimer();
}

void VideoPlayerImpl::onTextureReady(const Stream::VideoFrameParam &videoFrameParam)
{
    if (playerState_ == Stream::PlayerState::Stop || playerState_ == Stream::PlayerState::Pause)
        return;

    if ((playerState_ == Stream::PlayerState::Start || playerState_ == Stream::PlayerState::Resume)) {
        setPlayerState(Stream::PlayerState::Playing);
    }

    if (playerState_ == Stream::PlayerState::Playing) {
        strText_.clear();

        emit aboutToUpdate();
        emit ptsChanged(videoFrameParam.pts);
    }

    setFrameSize(videoFrameParam.size.width(), videoFrameParam.size.height());
}

void VideoPlayerImpl::onDisplayRendererReady(const QString &playerId, std::weak_ptr<DisplayRenderer> renderer)
{
    setDisplayRenderer(playerId, renderer);
}

void VideoPlayerImpl::onDisplayRendererAboutToDestroy(const QString &playerId)
{
    setDisplayRenderer(playerId, {});
}

void VideoPlayerImpl::onPlayerStateChanged(Stream::PlayerState state)
{
    switch (state) {
        case Stream::PlayerState::Start:
        case Stream::PlayerState::Resume:
            strText_ = tr("请稍候");
            break;
        case Stream::PlayerState::Stop:
            processParam_ = Stream::VideoProcessParam{};
            strText_.clear();

            setFrameSize(0, 0);

            videoRect_ = QRect();
            emit videoRectChanged(videoRect_);
            break;
        default:
            strText_.clear();
            break;
    }
}

void VideoPlayerImpl::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == updateStatisticalInfoTimerId_) {
        setupStatisticalInfo();
    }

    QObject::timerEvent(event);
}

QRect VideoPlayerImpl::calculateVideoRect(const QRect &needRenderedRect)
{
    if (frameWidth_ == 0 || frameHeight_ == 0) {
        return QRect();
    }

    const auto width = widgetRect_.width();
    const auto height = widgetRect_.height();

    QRect videoRect;
    if (aspectRatioMode() == Stream::AspectRatioMode::KeepAspectRatio) // 保持宽高比
    {
        if ((double)width / height >= (double)frameWidth_ / frameHeight_) // 被横向拉伸时，以高度为基准
        {
            const int nOffset = width - height * (double)frameWidth_ / frameHeight_;
            videoRect = QRect(needRenderedRect.x() + nOffset / 2, needRenderedRect.y(), width - nOffset, height);
        } else // 被纵向拉伸时，以宽度为基准
        {
            const int nOffset = height - width * (double)frameHeight_ / frameWidth_;
            videoRect = QRect(needRenderedRect.x(), needRenderedRect.y() + nOffset / 2, width, height - nOffset);
        }
    } else // 忽略宽高比
    {
        videoRect = needRenderedRect;
    }

    return videoRect;
}

void VideoPlayerImpl::setupStatisticalInfo()
{
    statisticalInfo_.clear();

    // 组装流信息
    if (streamInfo_.has_value()) {
        statisticalInfo_.append(QStringLiteral("基本信息\n"));
        statisticalInfo_.append(QStringLiteral("流地址：%1\n封装格式：%2\n")
                                    .arg(QString::fromStdString(streamInfo_->url), QString::fromStdString(streamInfo_->inputFormat)));
        if (streamInfo_->videoInfo.has_value()) {
            statisticalInfo_.append(QStringLiteral("视频流信息\n"));
            statisticalInfo_.append(QStringLiteral("帧大小：%1x%2(%3)，平均帧率：%4，颜色空间：%5\n")
                                        .arg(QString::number(streamInfo_->videoInfo->width),
                                             QString::number(streamInfo_->videoInfo->height),
                                             getAspectRatioString(streamInfo_->videoInfo->width, streamInfo_->videoInfo->height),
                                             QString::number(streamInfo_->videoInfo->frameRate),
                                             QString::fromStdString(streamInfo_->videoInfo->colorRange)));
        }
        if (streamInfo_->audioInfo.has_value()) {
            statisticalInfo_.append(QStringLiteral("音频流信息\n"));
            statisticalInfo_.append(QStringLiteral("采样率：%1，声道：%2\n")
                                        .arg(QString::number(streamInfo_->audioInfo->sampleRate),
                                             QString::number(streamInfo_->audioInfo->channels)));
        }

        statisticalInfo_.append(QLatin1Char('\n'));
    } else {
        statisticalInfo_.append(QStringLiteral("基本信息 - 暂无\n"));
    }

    // 组装解码器信息
    if (decoderInfos_.contains(decoder_sdk::MediaType::kVideo) &&
        decoderInfos_[decoder_sdk::MediaType::kVideo].has_value()) {
        const auto &videoDecoderInfo = decoderInfos_[decoder_sdk::MediaType::kVideo];
        statisticalInfo_.append(QStringLiteral("视频解码器信息\n"));
        statisticalInfo_.append(QStringLiteral("输入\n"));
        statisticalInfo_.append(QStringLiteral("    |--解码器名称：%1，硬件加速类型：%2\n")
                                    .arg(QString::fromStdString(videoDecoderInfo->codecName),
                                         QString::fromStdString(decoder_sdk::getHwAccelTypeDesc(videoDecoderInfo->hwAccelType))));

        statisticalInfo_.append(QStringLiteral("输出\n"));
        statisticalInfo_.append(QStringLiteral("    |--渲染器名称：%1\n").arg(rendererName_));
        statisticalInfo_.append(QStringLiteral("    |--渲染设备：%1\n").arg(getCurrentGLRenderer()));
        statisticalInfo_.append(QStringLiteral("    |--输出窗口大小：%1x%2(%3)，视频帧大小：%4x%5(%6)\n")
                                    .arg(QString::number(widgetRect_.width()),
                                         QString::number(widgetRect_.height()),
                                         getAspectRatioString(widgetRect_.width(), widgetRect_.height()),
                                         QString::number(videoRect_.width()),
                                         QString::number(videoRect_.height()),
                                         getAspectRatioString(videoRect_.width(), videoRect_.height())));

        statisticalInfo_.append(QLatin1Char('\n'));
    } else {
        statisticalInfo_.append(QStringLiteral("视频解码器信息 - 暂无\n"));
    }

    if (decoderInfos_.contains(decoder_sdk::MediaType::kAudio) &&
        decoderInfos_[decoder_sdk::MediaType::kAudio].has_value()) {
        const auto &audioDecoderInfo = decoderInfos_[decoder_sdk::MediaType::kAudio];
        statisticalInfo_.append(QStringLiteral("音频解码器信息\n"));
        statisticalInfo_.append(QStringLiteral("输入\n"));
        statisticalInfo_.append(QStringLiteral("    |--解码器名称：%1\n").arg(QString::fromStdString(audioDecoderInfo->codecName)));
    } else {
        statisticalInfo_.append(QStringLiteral("音频解码器信息 - 暂无\n"));
    }

    // 关闭定时器
    if (updateStatisticalInfoTimerId_ >= 0) {
        killTimer(updateStatisticalInfoTimerId_);
        updateStatisticalInfoTimerId_ = -1;
    }
}

void VideoPlayerImpl::resetStatisticalInfoUpdateTimer()
{
    if (updateStatisticalInfoTimerId_ >= 0) {
        killTimer(updateStatisticalInfoTimerId_);
        updateStatisticalInfoTimerId_ = -1;
    }

    updateStatisticalInfoTimerId_ = startTimer(std::chrono::milliseconds(1));
}

void VideoPlayerImpl::setFrameSize(int width, int height)
{
    if (width == frameWidth_ && height == frameHeight_)
        return;

    frameWidth_ = width;
    frameHeight_ = height;
    emit frameSizeChanged(width, height);
}

void VideoPlayerImpl::setDisplayRenderer(const QString &playerId, std::weak_ptr<DisplayRenderer> render)
{
    if (playerId != uuid_)
        return;

    render_ = render;
}