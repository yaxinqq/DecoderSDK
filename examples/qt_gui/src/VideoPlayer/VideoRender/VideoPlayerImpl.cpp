#include "VideoPlayerImpl.h"
#include "RenderWorker.h"
#include "StreamDecodeWorker.h"

#include <QDebug>
#include <QFontMetrics>
#include <QTimer>
#include <QGuiApplication>
#include <QThread>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOffscreenSurface>
#include <QPainter>
#include <QOpenGLFramebufferObject>

VideoPlayerImpl::VideoPlayerImpl(QObject *parent)
	: QObject(parent), playerState_{Stream::PlayerState::Stop}, strText_{}
{
	QTimer *timer = new QTimer(this);
	timer->setTimerType(Qt::PreciseTimer);
	timer->setInterval(1000);
	connect(timer, &QTimer::timeout, [this]()
			{
		fps_ = frameCount_;
		frameCount_ = 0;
		emit aboutToUpdate(); });
	timer->start();

	painterFont_ = qGuiApp->font();
	painterFont_.setPixelSize(30);
	painterFont_.setWeight(QFont::Bold);
	QFontMetrics fm(painterFont_);
	fpsTextWidth_ = fm.horizontalAdvance(QString("FPS: 99")) + 10;
	fpsTextHeight_ = fm.height();

	// fpsVisible_ = GlobalParams::instance()->fpsVisible();
	// connect(GlobalParams::instance(), &GlobalParams::fpsVisibleChanged, this, [this](bool visible) {
	// 	fpsVisible_ = visible;
	// 	emit aboutToUpdate();
	// });

	// 流地址打开超时
	streamOpenTimeout_.store(false);
    streamOpenErrorTimer_ = new QTimer{this};
    streamOpenErrorTimer_->setSingleShot(true);
    streamOpenErrorTimer_->setInterval(18000);
    connect(streamOpenErrorTimer_, &QTimer::timeout, this, [this]() {
        streamOpenTimeout_.store(true);
        emit streamClosed();
    });
	//================

	connect(this, &VideoPlayerImpl::playerStateChanged, this, &VideoPlayerImpl::onPlayerStateChanged);
}

VideoPlayerImpl::~VideoPlayerImpl()
{
	if (renderWorker_)
	{
		renderWorker_->deleteLater();
	}
	if (renderWorkerThread_)
	{
		qDebug() << __FUNCTION__ << "Render worker begin quit!";
		renderWorkerThread_->requestInterruption();
		renderWorkerThread_->quit();
		if (!renderWorkerThread_->wait(3000))
		{
			// Todo: 分析一下为什么需要 terminate
			qDebug() << __FUNCTION__ << "Render worker begin terminated!";
			renderWorkerThread_->terminate();
			renderWorkerThread_->wait();
			qDebug() << __FUNCTION__ << "Render worker end terminated!";
		}
		qDebug() << __FUNCTION__ << "Render worker end quit!";
	}
}

void VideoPlayerImpl::initialize(QOpenGLContext *context, QSurface *surface)
{
	if (!renderWorker_)
	{
		auto renderSurface = new QOffscreenSurface(nullptr, this);
		renderSurface->setFormat(surface->format());
		renderSurface->create();

		renderWorker_ = new RenderWorker(renderSurface, context);
		renderWorkerThread_ = new QThread();
		// connect(renderWorkerThread_, &QThread::finished, renderWorker_, &RenderWorker::deleteLater);
		connect(renderWorkerThread_, &QThread::finished, renderWorkerThread_, &QThread::deleteLater);
		renderWorker_->moveToThread(renderWorkerThread_);

		connect(this, &VideoPlayerImpl::renderRequested, renderWorker_, &RenderWorker::render);
		connect(this, &VideoPlayerImpl::prepareStop, renderWorker_, &RenderWorker::prepareStop);
		connect(this, &VideoPlayerImpl::preparePause, renderWorker_, &RenderWorker::preparePause);
		connect(this, &VideoPlayerImpl::preparePlaying, renderWorker_, &RenderWorker::preparePlaying);

		renderWorkerThread_->start();

		connect(renderWorker_, &RenderWorker::textureReady, [this](QWeakPointer<VideoRender> render)
				{
			if (playerState_ == Stream::PlayerState::Playing)
			{
				render_ = render;
				emit aboutToUpdate();
			} });
	}
}

void VideoPlayerImpl::paintGL(const QRect &widgetRect, const QRect &needRenderedRect, const QPoint &referencePt)
{
	// widgetRect_ = widgetRect;
	QOpenGLContext *curContext = QOpenGLContext::currentContext();
	if (!curContext)
		return;

	// 黑色背景
	clear(needRenderedRect, referencePt);

	if (!render_)
		return;

	if (playerState_ == Stream::PlayerState::Playing || playerState_ == Stream::PlayerState::Pause)
	{
		if (playerState_ == Stream::PlayerState::Playing)
			frameCount_++;

		const QRect videoRect = calculateVideoRect(needRenderedRect);
		// 调整视频绘制区域
		const auto diff = videoRect.bottomLeft() - referencePt;
		curContext->functions()->glViewport(diff.x(), -diff.y(), videoRect.width(), videoRect.height());

		if (videoRect != videoRect_)
		{
			videoRect_ = videoRect;
			emit videoRectChanged(videoRect_);
			// qDebug() << __FUNCTION__ << "widget rect:" << widgetRect_ << "video rect:" << videoRect_ << "frame:" << frameWidth_ << "x" << frameHeight_;
		}

		// 渲染帧
		if (!render_.isNull())
		{
			auto sharedRender = render_.lock();
			if (sharedRender)
				sharedRender->draw();
		}
	}
}

void VideoPlayerImpl::resize(int w, int h)
{
	const QRect widgetRect = QRect(0, 0, w, h);
	if (widgetRect != widgetRect_)
	{
		widgetRect_ = widgetRect;
		emit widgetRectChanged(widgetRect_);
		// qDebug() << __FUNCTION__ << "widget rect:" << widgetRect_ << "video rect:" << videoRect_ << "frame:" << frameWidth_ << "x" << frameHeight_;
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
	// return GlobalParams::instance()->videoKeepAspectRatio();
	return Stream::AspectRatioMode::KeepAspectRatio;
}

void VideoPlayerImpl::paintCommon(QPainter *painter, const QRect &widgetRect)
{
	// widgetRect_ = widgetRect;

	painter->save();
	painter->setFont(painterFont_);

	if ((playerState() == Stream::PlayerState::Pause || playerState() == Stream::PlayerState::Resume) && !lastFrame_.isNull())
	{
		const QRect imageDrawRect = calculateVideoRect(widgetRect_);
		painter->drawImage(imageDrawRect, lastFrame_);
	}

	painter->setPen(Qt::white);
	painter->drawText(widgetRect_, Qt::AlignCenter, strText_);

	if (fpsVisible_)
	{
		painter->setPen(QColor(255, 242, 0));
		painter->drawText(widgetRect_.topRight() - QPoint(fpsTextWidth_, -fpsTextHeight_), QString("FPS: %1").arg(fps_));
	}

	if (playerState_ == Stream::PlayerState::Playing || playerState_ == Stream::PlayerState::Pause)
	{
		for (auto img : masks_)
			painter->drawImage(widgetRect_, *img);
	}
	painter->restore();
}

void VideoPlayerImpl::videoFrameReady(const decoder_sdk::Frame &frame)
{
	if (playerState_ == Stream::PlayerState::Stop || playerState_ == Stream::PlayerState::Pause)
		return;

	if ((playerState_ == Stream::PlayerState::Start || playerState_ == Stream::PlayerState::Resume) && frame.isValid())
	{
		setPlayerState(Stream::PlayerState::Playing);
	}

	if (playerState_ == Stream::PlayerState::Playing && frame.isValid())
	{
		frameWidth_ = frame.width();
		frameHeight_ = frame.height();

		emit renderRequested(frame);
		strText_.clear();
	}
}

void VideoPlayerImpl::renderToImage(const QSize &size, QImage &image)
{
	QOpenGLContext *curContext = QOpenGLContext::currentContext();
	QOpenGLContext *renderContext = renderWorker_ ? renderWorker_->context() : nullptr;
	if (!renderContext)
		return;

	// 找到当前线程中和renderWorker的共享上下文，如果没有就返回
	const auto sharedContexts = renderContext->shareGroup()->shares();
	QOpenGLContext *curThreadRenderWorkerContext = nullptr;
	for (auto *const context : sharedContexts)
	{
		if (context->thread() == QThread::currentThread())
		{
			curThreadRenderWorkerContext = context;
			break;
		}
	}
	if (!curThreadRenderWorkerContext)
		return;

	// 渲染到帧缓冲，并生成图像
	if (!render_.isNull())
	{
		auto sharedRender = render_.lock();
		if (sharedRender)
		{
			curThreadRenderWorkerContext->makeCurrent(renderWorker_->surface());
			curThreadRenderWorkerContext->functions()->glViewport(0, 0, size.width(), size.height());
			auto frameBuffer = sharedRender->getFrameBuffer();
			if (frameBuffer)
			{
				image = frameBuffer->toImage();
				// 清理帧对象
				GLuint fbo = frameBuffer->handle(); // 返回此帧缓冲对象的OpenGL帧缓冲对象句柄
				curThreadRenderWorkerContext->functions()->glDeleteFramebuffers(1, &fbo);

				frameBuffer.reset();
			}
			curThreadRenderWorkerContext->doneCurrent();

			// 恢复之前的上下文
			if (curContext)
				curContext->makeCurrent(curContext->surface());
		}
	}
	else
	{
		// 没有渲染器时，使用上次保存的图片
		image = lastFrame_;
	}
}

void VideoPlayerImpl::renderToImage(QImage &image)
{
	renderToImage(QSize(frameWidth_, frameHeight_), image);
}

void VideoPlayerImpl::setPlayerState(Stream::PlayerState state)
{
	playerState_ = state;
#ifndef QT_COMMERCIAL
	emit playerStateChanged(playerState_);
#endif

	if (playerState_ == Stream::PlayerState::Playing)
	{
		emit preparePlaying();
	}
	else if (playerState_ == Stream::PlayerState::Pause)
	{
		renderToImage(lastFrame_); // 获得最后一帧

		emit preparePause();
	}
	else if (playerState_ == Stream::PlayerState::Stop)
	{
		emit prepareStop();
		streamOpenTimeout_.store(false);
		if (streamOpenErrorTimer_->isActive())
			streamOpenErrorTimer_->stop();
	}

	emit aboutToUpdate();
}

void VideoPlayerImpl::onDecoderEventChanged(decoder_sdk::EventType type,
	const std::shared_ptr<decoder_sdk::EventArgs> &event)
{
    switch (type) {
        case decoder_sdk::EventType::kStreamOpenFailed:
            strText_ = QStringLiteral("流地址无效");
            // 如果之前未设置超时状态，则开启超时计时器，如果想不停的尝试重连可以把
            // !streamOpenTimeout_.load() 条件去掉
            if (!streamOpenErrorTimer_->isActive() && !streamOpenTimeout_.load())
                streamOpenErrorTimer_->start();
            break;
        case decoder_sdk::EventType::kStreamOpened:
            // 关闭超时计时器，并修改bool值
            if (streamOpenErrorTimer_->isActive())
                streamOpenErrorTimer_->stop();

            streamOpenTimeout_.store(false);
            break;
        case decoder_sdk::EventType::kCreateDecoderFailed:
            strText_ = QStringLiteral("资源不足");
            qWarning() << QStringLiteral("播放失败，解码器资源不足。");
            break;
        case decoder_sdk::EventType::kRecordingStarted:
            emit recordStarted();
            break;
        case decoder_sdk::EventType::kRecordingStopped:
            emit recordStopped();
            break;
        case decoder_sdk::EventType::kRecordingError:
            emit recordErrorOccured();
            break;
        default:
            break;
    }
}

void VideoPlayerImpl::onPlayerStateChanged(Stream::PlayerState state)
{
	switch (state)
	{
	case Stream::PlayerState::Start:
	case Stream::PlayerState::Resume:
		strText_ = QStringLiteral("请稍候");
		break;
	case Stream::PlayerState::Stop:
	default:
		strText_.clear();
		break;
	}
}

QRect VideoPlayerImpl::calculateVideoRect(const QRect &needRenderedRect)
{
	const auto width = widgetRect_.width();
	const auto height = widgetRect_.height();

	QRect videoRect;
	if (aspectRatioMode() == Stream::AspectRatioMode::KeepAspectRatio) // 保持宽高比
	{
		if ((double)width / height >= (double)frameWidth_ / frameHeight_) // 被横向拉伸时，以高度为基准
		{
			const int nOffset = width - height * (double)frameWidth_ / frameHeight_;
			videoRect = QRect(needRenderedRect.x() + nOffset / 2, needRenderedRect.y(), width - nOffset, height);
		}
		else // 被纵向拉伸时，以宽度为基准
		{
			const int nOffset = height - width * (double)frameHeight_ / frameWidth_;
			videoRect = QRect(needRenderedRect.x(), needRenderedRect.y() + nOffset / 2, width, height - nOffset);
		}
	}
	else // 忽略宽高比
	{
		videoRect = needRenderedRect;
	}

	return videoRect;
}
