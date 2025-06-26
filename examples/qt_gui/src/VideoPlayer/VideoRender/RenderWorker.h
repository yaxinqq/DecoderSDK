#pragma once

#include "VideoRender.h"
#include "decodersdk/common_define.h"

#include <QObject>
#include <QSharedPointer>

class QThread;
class QSurface;
class QOpenGLContext;

class RenderWorker : public QObject
{
	Q_OBJECT

public:
	RenderWorker(QSurface *surface, QOpenGLContext *context, QObject *parent = nullptr);
	~RenderWorker();

	QOpenGLContext *context() { return context_; }
	QSurface *surface() { return surface_; }

signals:
	void textureReady(const QWeakPointer<VideoRender> &render);

public slots:
	void render(const decoder_sdk::Frame &frame);
	void prepareStop();
	void preparePause();
	void preparePlaying();

private:
	// 根据像素格式创建对应的渲染器
	QSharedPointer<VideoRender> createRenderer(decoder_sdk::ImageFormat format);
	
	QSharedPointer<VideoRender> render_;
	QSurface *surface_;
	QOpenGLContext *context_;

	int renderWidth_ = 0;
	int renderHeight_ = 0;
	decoder_sdk::ImageFormat currentPixelFormat_ = decoder_sdk::ImageFormat::kUnknown;

	// 是否准备好渲染，当播放器暂停时，该状态为false
	std::atomic<bool> readyRender_;
};
