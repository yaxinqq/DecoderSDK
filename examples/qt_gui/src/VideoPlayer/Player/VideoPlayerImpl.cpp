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
#include <utility>

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

    QRect toDevicePixelRect(const QPoint &origin, const QSize &size, qreal devicePixelRatio)
    {
        const qreal scale = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
        const int left = qRound(origin.x() * scale);
        const int top = qRound(origin.y() * scale);
        const int right = qRound((origin.x() + size.width()) * scale);
        const int bottom = qRound((origin.y() + size.height()) * scale);
        return QRect(left, top, qMax(0, right - left), qMax(0, bottom - top));
    }

    // 将比特率(bps)格式化为人类可读的字符串
    QString formatBitrate(const int bitrate)
    {
        if (bitrate <= 0) {
            return QStringLiteral("未知");
        }
        if (bitrate >= 1000000) {
            return QStringLiteral("%1 Mbps").arg(bitrate / 1000000.0, 0, 'f', 2);
        }
        if (bitrate >= 1000) {
            return QStringLiteral("%1 kbps").arg(bitrate / 1000.0, 0, 'f', 2);
        }
        return QStringLiteral("%1 bps").arg(bitrate);
    }

    // 计算视频压缩率（以解码器实际输出像素格式的未压缩数据率为基准）
    // bpp 由解码器基于 av_image_get_buffer_size 精确计算一次后提供
    QString calculateCompressionRatio(const int width, const int height, const int frameRate, const int bitrate,
                                      const int bpp)
    {
        if (width <= 0 || height <= 0 || frameRate <= 0 || bitrate <= 0 || bpp <= 0) {
            return QStringLiteral("未知");
        }
        // 未压缩数据率 = 宽 × 高 × bpp × 帧率
        const double rawBitrate = static_cast<double>(width) * height * bpp * frameRate;
        const double ratio = rawBitrate / bitrate;
        return QStringLiteral("%1:1").arg(ratio, 0, 'f', 2);
    }

    // 构建媒体源的基础信息；streamInfo为空时返回统一的暂无占位文案
    QString buildBasicStatisticsInfo(const std::optional<decoder_sdk::StreamInfo> &streamInfo)
    {
        if (!streamInfo.has_value()) {
            return QStringLiteral("基本信息 - 暂无\n");
        }

        QString info = QStringLiteral("基本信息\n");
        info += QStringLiteral("流地址：%1\n封装格式：%2\n")
                    .arg(QString::fromStdString(streamInfo->url),
                         QString::fromStdString(streamInfo->inputFormat));

        if (streamInfo->videoInfo.has_value()) {
            info += QStringLiteral("视频流信息\n");
            info += QStringLiteral("帧大小：%1x%2(%3)，平均帧率：%4，颜色空间：%5\n")
                        .arg(QString::number(streamInfo->videoInfo->width),
                             QString::number(streamInfo->videoInfo->height),
                             getAspectRatioString(streamInfo->videoInfo->width, streamInfo->videoInfo->height),
                             QString::number(streamInfo->videoInfo->frameRate),
                             QString::fromStdString(streamInfo->videoInfo->colorRange));
        }

        if (streamInfo->audioInfo.has_value()) {
            info += QStringLiteral("音频流信息\n");
            info += QStringLiteral("采样率：%1，声道：%2\n")
                        .arg(QString::number(streamInfo->audioInfo->sampleRate),
                             QString::number(streamInfo->audioInfo->channels));
        }

        info += QLatin1Char('\n');
        return info;
    }

    // 构建解码器输入与播放端输出信息；只展示已经收到过的音视频解码器
    QString buildDecoderStatisticsInfo(const QMap<decoder_sdk::MediaType, std::optional<decoder_sdk::DecoderInfo>> &decoderInfos,
                                       const QString &rendererName, const QRect &widgetRect, const QRect &videoRect)
    {
        QString info;
        const auto videoDecoderInfo = decoderInfos.constFind(decoder_sdk::MediaType::kVideo);
        if (videoDecoderInfo != decoderInfos.constEnd() && videoDecoderInfo->has_value()) {
            info += QStringLiteral("视频解码器信息\n");
            info += QStringLiteral("输入\n");
            info += QStringLiteral("    |--解码器名称：%1，硬件加速类型：%2\n")
                        .arg(QString::fromStdString((*videoDecoderInfo)->codecName),
                             QString::fromStdString(decoder_sdk::getHwAccelTypeDesc((*videoDecoderInfo)->hwAccelType)));

            info += QStringLiteral("输出\n");
            info += QStringLiteral("    |--渲染器名称：%1\n").arg(rendererName);
            info += QStringLiteral("    |--渲染设备：%1\n").arg(getCurrentGLRenderer());
            info += QStringLiteral("    |--输出窗口大小：%1x%2(%3)，视频帧大小：%4x%5(%6)\n")
                        .arg(QString::number(widgetRect.width()),
                             QString::number(widgetRect.height()),
                             getAspectRatioString(widgetRect.width(), widgetRect.height()),
                             QString::number(videoRect.width()),
                             QString::number(videoRect.height()),
                             getAspectRatioString(videoRect.width(), videoRect.height()));
            info += QLatin1Char('\n');
        } else {
            info += QStringLiteral("视频解码器信息 - 暂无\n");
        }

        const auto audioDecoderInfo = decoderInfos.constFind(decoder_sdk::MediaType::kAudio);
        if (audioDecoderInfo != decoderInfos.constEnd() && audioDecoderInfo->has_value()) {
            info += QStringLiteral("音频解码器信息\n");
            info += QStringLiteral("输入\n");
            info += QStringLiteral("    |--解码器名称：%1\n")
                        .arg(QString::fromStdString((*audioDecoderInfo)->codecName));
        } else {
            info += QStringLiteral("音频解码器信息 - 暂无\n");
        }
        return info;
    }

    // 构建实时码率统计；缺少视频信息时不进行压缩率计算
    QString buildRealtimeStatisticsInfo(const std::optional<decoder_sdk::StreamInfo> &streamInfo,
                                        const QMap<decoder_sdk::MediaType, std::optional<decoder_sdk::DecoderInfo>> &decoderInfos,
                                        const decoder_sdk::StreamStaticsInfo &streamStaticsInfo)
    {
        if (!streamInfo.has_value()) {
            return QStringLiteral("实时统计信息 - 暂无\n");
        }

        const auto videoDecoderInfo = decoderInfos.constFind(decoder_sdk::MediaType::kVideo);
        const int bpp = videoDecoderInfo != decoderInfos.constEnd() && videoDecoderInfo->has_value()
                            ? (*videoDecoderInfo)->bitsPerPixel
                            : 0;

        QString compressionRatio = QStringLiteral("未知");
        if (streamInfo->videoInfo.has_value()) {
            compressionRatio = calculateCompressionRatio(streamInfo->videoInfo->width,
                                                        streamInfo->videoInfo->height,
                                                        streamInfo->videoInfo->frameRate,
                                                        streamStaticsInfo.videoBitrate,
                                                        bpp);
        }

        QString info = QStringLiteral("\n实时统计信息\n");
        info += QStringLiteral("比特率：%1，压缩率：%2\n")
                    .arg(formatBitrate(streamStaticsInfo.videoBitrate), compressionRatio);
        return info;
    }
} // namespace

VideoPlayerImpl::VideoPlayerImpl(QObject *parent)
    : QObject(parent)
    , playerState_{ Stream::PlayerState::Stop }
    , strText_{}
    , uuid_{ QUuid::createUuid().toString(QUuid::StringFormat::WithoutBraces) }
{
    QTimer *timer = new QTimer(this);
    timer->setInterval(1000);
    timer->setTimerType(Qt::VeryCoarseTimer);
    connect(timer, &QTimer::timeout, this, &VideoPlayerImpl::onFpsTimerTimeout);
    timer->start();
    resetFpsStatistics();

    painterFont_ = qGuiApp->font();
    painterFont_.setPixelSize(30);
    painterFont_.setWeight(QFont::Bold);
    QFontMetrics fm(painterFont_);
    fpsTextWidth_ = fm.horizontalAdvance(QStringLiteral("渲染FPS: 999，绘制FPS: 999")) + 10;
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

void VideoPlayerImpl::paintGL(const QRect &widgetRect, const QRect &needRenderedRect,
                              const QPoint &referencePt, qreal devicePixelRatio)
{
    // widgetRect_ = widgetRect;
    QOpenGLContext *curContext = QOpenGLContext::currentContext();
    if (!curContext)
        return;

    // 黑色背景
    clear(needRenderedRect, referencePt, devicePixelRatio);
    ++paintFrameCount_;

    if (render_.expired())
        return;

    if (playerState_ == Stream::PlayerState::Playing || playerState_ == Stream::PlayerState::Pause) {
        QRect renderedVideoRect = calculateVideoRect(needRenderedRect);
        // 调整视频绘制区域
        const auto diff = renderedVideoRect.bottomLeft() - referencePt;
        const QRect deviceViewportRect =
            toDevicePixelRect(QPoint(diff.x(), -diff.y()), renderedVideoRect.size(), devicePixelRatio);
        curContext->functions()->glViewport(deviceViewportRect.x(), deviceViewportRect.y(),
                                            deviceViewportRect.width(), deviceViewportRect.height());

        // 调整到自身坐标系
        const QRect localVideoRect = calculateVideoRect(widgetRect_);
        if (localVideoRect != videoRect_) {
            // 在实际赋值时，videoRect应该保持自身坐标系
            videoRect_ = localVideoRect;
            emit videoRectChanged(videoRect_);
            invalidateStatistics();
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

        invalidateStatistics();
    }
}

void VideoPlayerImpl::clear(const QRect &needRenderedRect, const QPoint &referencePt, qreal devicePixelRatio)
{
    QOpenGLContext *curContext = QOpenGLContext::currentContext();
    if (!curContext)
        return;

    // 清屏，绘制黑色背景
    auto *const f = curContext->functions();

    const auto diff = needRenderedRect.bottomLeft() - referencePt;
    const QRect deviceRect =
        toDevicePixelRect(QPoint(diff.x(), -diff.y()), needRenderedRect.size(), devicePixelRatio);
    f->glViewport(deviceRect.x(), deviceRect.y(), deviceRect.width(), deviceRect.height());

    // 启用裁剪
    f->glEnable(GL_SCISSOR_TEST);
    f->glScissor(deviceRect.x(), deviceRect.y(), deviceRect.width(), deviceRect.height());

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

        // updateStatisticsIfDirty();

        // // 绘制统计信息
        // painter->fillRect(widgetRect, QColor(0, 0, 0, 127));

        // QTextOption textOpt;
        // textOpt.setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        // textOpt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        // painter->drawText(widgetRect.adjusted(30, 0, 0, 0), statisticalInfo_, textOpt);

        // 绘制FPS
        painter->drawText(widgetRect_.topRight() - QPoint(fpsTextWidth_, -fpsTextHeight_),
                          QStringLiteral("渲染FPS: %1，绘制FPS: %2").arg(renderFps_).arg(paintFps_));
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

void VideoPlayerImpl::resetFpsStatistics()
{
    renderFrameCount_ = 0;
    paintFrameCount_ = 0;
    renderFps_ = 0;
    paintFps_ = 0;
    fpsWindowTimer_.restart();
}

void VideoPlayerImpl::setPlayerState(Stream::PlayerState state)
{
    if (playerState_ == state)
        return;

    // 只在新进入Playing或Stop时重建FPS窗口，重复的PLAY状态不丢弃统计样本。
    const bool shouldResetFps = state == Stream::PlayerState::Stop ||
                                (state == Stream::PlayerState::Playing &&
                                 playerState_ != Stream::PlayerState::Playing);

    playerState_ = state;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    emit playerStateChanged(playerState_);
#endif

    if (shouldResetFps) {
        resetFpsStatistics();
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
                invalidateStatistics();

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
                invalidateStatistics();
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
            emit recordErrored(utils::getFilePathFromRecordEvent(event));
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
    invalidateStatistics();
}

void VideoPlayerImpl::onStreamStaticsInfoUpdated(const decoder_sdk::StreamStaticsInfo &info)
{
    streamStaticsInfo_ = info;
    invalidateStatistics();
}

void VideoPlayerImpl::onDecoderInfoUpdated(decoder_sdk::MediaType mediaType, const std::optional<decoder_sdk::DecoderInfo> &info)
{
    decoderInfos_.insert(mediaType, info);
    invalidateStatistics();
}

void VideoPlayerImpl::onRendererNameChanged(const QString &name)
{
    rendererName_ = name;
    invalidateStatistics();
}

void VideoPlayerImpl::onTextureReady(const Stream::VideoFrameParam &videoFrameParam)
{
    if ((playerState_ == Stream::PlayerState::Start || playerState_ == Stream::PlayerState::Resume)) {
        setPlayerState(Stream::PlayerState::Playing);
    }

    // textureReady由Renderer在成功绘制后发出，因此这里统计的是实际渲染帧，而非界面重绘帧。
    if (playerState_ != Stream::PlayerState::Stop) {
        ++renderFrameCount_;
    }

    if (playerState_ == Stream::PlayerState::Stop || playerState_ == Stream::PlayerState::Pause)
        return;

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

void VideoPlayerImpl::onFpsTimerTimeout()
{
    // QTimer只能保证大致按周期触发；用单调时钟的真实间隔换算，避免UI线程繁忙时低估FPS。
    const qint64 elapsedMs = fpsWindowTimer_.elapsed();
    const double elapsedSeconds = elapsedMs / 1000.0;

    renderFps_ = elapsedSeconds > 0.0 ? qRound(renderFrameCount_ / elapsedSeconds) : 0;
    paintFps_ = elapsedSeconds > 0.0 ? qRound(paintFrameCount_ / elapsedSeconds) : 0;

    renderFrameCount_ = 0;
    paintFrameCount_ = 0;
    fpsWindowTimer_.restart();
    emit aboutToUpdate();
}

QRect VideoPlayerImpl::calculateVideoRect(const QRect &needRenderedRect)
{
    if (frameWidth_ == 0 || frameHeight_ == 0) {
        return QRect();
    }

    const auto width = needRenderedRect.width();
    const auto height = needRenderedRect.height();

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

void VideoPlayerImpl::updateStatisticsIfDirty()
{
    if (!statisticsDirty_) {
        return;
    }

    QString info;
    info.reserve(2048);
    info += buildBasicStatisticsInfo(streamInfo_);
    info += buildDecoderStatisticsInfo(decoderInfos_, rendererName_, widgetRect_, videoRect_);
    info += buildRealtimeStatisticsInfo(streamInfo_, decoderInfos_, streamStaticsInfo_);
    statisticalInfo_ = std::move(info);
    statisticsDirty_ = false;
}

void VideoPlayerImpl::invalidateStatistics()
{
    statisticsDirty_ = true;
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
