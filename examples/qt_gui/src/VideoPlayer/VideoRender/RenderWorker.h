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
    void textureReady(const QWeakPointer<VideoRender> &render,
                      const Stream::VideoFrameParam &videoFrameParam);

public slots:
    void render(const std::shared_ptr<decoder_sdk::Frame> &frame,
                const Stream::VideoProcessParam &processParam);
    void prepareStop();
    void preparePause();
    void preparePlaying();

private:
    /**
     * @brief 渲染音频
     *
     * @param audioFrame 音频帧
     */
    void renderAudio(const std::shared_ptr<decoder_sdk::Frame> &audioFrame);
    /**
     * @brief 渲染视频
     *
     * @param videoFrame 视频帧
     * @param processParam 视频处理参数
     */
    void renderVideo(const std::shared_ptr<decoder_sdk::Frame> &videoFrame,
                     const Stream::VideoProcessParam &processParam);

    /**
     * @brief 处理视频帧中的SEI数据
     *
     * @param videoFrame 视频帧
     */
    void handleFrameSEI(const std::shared_ptr<decoder_sdk::Frame> &videoFrame);

    /**
     * @brief 创建视频渲染器
     *
     * @param videoFrame 视频帧
     * @return QSharedPointer<VideoRender> 视频渲染器
     */
    QSharedPointer<VideoRender> createRenderer(
        const std::shared_ptr<decoder_sdk::Frame> &videoFrame);

    /**
     * @brief 确保上下文已是当前上下文
     *
     * @return true 成功
     */
    bool ensureContextCurrent();
    /**
     * @brief 释放当前上下文
     */
    void releaseContextCurrent();

    /**
     * @brief 停止并释放音频渲染器
     */
    void stopAndReleaseAudioRender();

    /**
     * @brief 释放视频渲染器
     */
    void releaseVideoRender();

private:
    // 渲染表面
    QSurface *surface_;
    // 渲染上下文
    QOpenGLContext *context_;

    // 视频部分
    // 视频渲染器
    QSharedPointer<VideoRender> render_;
    // 视频帧参数（最终结果，会考虑裁剪拼接）
    Stream::VideoFrameParam videoFrameParam_;
    // 视频帧原始宽度
    int frameOriginWidth_ = 0;
    // 视频帧原始高度
    int frameOriginHeight_ = 0;
    // 视频帧图像格式
    decoder_sdk::ImageFormat currentPixelFormat_ = decoder_sdk::ImageFormat::kUnknown;

    // 音频部分
    // 音频渲染器
    std::unique_ptr<AudioRender> audioRender_;
    // 音频参数缓存，用于判断是否需要重新初始化
    int audioSampleRate_ = 0;
    int audioChannels_ = 0;
    decoder_sdk::AudioSampleFormat audioSampleFormat_ = decoder_sdk::AudioSampleFormat::kUnknown;
};
