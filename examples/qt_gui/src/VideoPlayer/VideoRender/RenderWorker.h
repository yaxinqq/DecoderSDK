#pragma once

#include "AudioRender.h"
#include "VideoRender.h"
#include "decodersdk/common_define.h"

#include <QObject>
#include <QSharedPointer>

class QThread;
class QSurface;
class QOpenGLContext;

class RenderWorker : public QObject {
    Q_OBJECT

public:
    RenderWorker(QSurface *surface, QOpenGLContext *context, QObject *parent = nullptr);
    ~RenderWorker();

    QOpenGLContext *context()
    {
        return context_;
    }
    QSurface *surface()
    {
        return surface_;
    }

    /**
     * @brief 设置音量 (0.0 - 1.0)
     */
    void setVolume(qreal volume);

    /**
     * @brief 获取音量
     */
    qreal volume() const;

signals:
    void textureReady(const QWeakPointer<VideoRender> &render, double pts);

public slots:
    void render(const std::shared_ptr<decoder_sdk::Frame> &frame);
    void prepareStop();
    void preparePause();
    void preparePlaying();

private:
    void renderAudio(const std::shared_ptr<decoder_sdk::Frame> &audioFrame);
    void renderVideo(const std::shared_ptr<decoder_sdk::Frame> &videoFrame);
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

    // 音频部分
    std::unique_ptr<AudioRender> audioRender_;

    // 音频参数缓存，用于判断是否需要重新初始化
    int audioSampleRate_ = 0;
    int audioChannels_ = 0;
    decoder_sdk::AudioSampleFormat audioSampleFormat_ = decoder_sdk::AudioSampleFormat::kUnknown;
};
