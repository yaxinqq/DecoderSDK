#pragma once

#include "VideoRender/VideoPlayer.h"

class RtspStreamPlayer : public VideoPlayer {
    Q_OBJECT

public:
    explicit RtspStreamPlayer(QWidget *parent = nullptr);
    virtual ~RtspStreamPlayer();

public:
    // 视频流地址
    QString streamUrl() const
    {
        return streamUrl_;
    }
    // 视频流设备Id
    QString deviceId() const
    {
        return deviceId_;
    }
    // 视频流通道Id（可能为空，若是第三方设备，则通道ID为空）
    QString channelId() const
    {
        return channelId_;
    }

    // 打开视频流
    virtual void open(const QString &url, const QString &deviceId, const QString &channelId,
                      Stream::OpenMode openMode = Stream::OpenMode::kExclusive);
    // 关闭视频流
    virtual void close();
    // 暂停播放视频
    virtual void pause() override;
    // 恢复播放视频
    virtual void resume() override;

    // 切换解码器，如果当前没有url对应的解码器，会先尝试创建。切换成功返回true，失败返回false
    // 适用于一个播放介质对应同一个设备的多个解码器（如球机对应可见光视频流和红外视频流）
    // 调用此函数前，要保证deviceId、channelId、openMode是有效的
    virtual bool switchStream(const QString &url);

signals:
    // 正在播放的离线设备又上线的信号，应用层需要恢复播放
    void offlinedDeviceOnline();

protected:
    QString streamUrl_;
    QString deviceId_;
    QString channelId_;
    Stream::OpenMode openMode_ = Stream::OpenMode::kExclusive; // 打开方式，默认独占打开

    // 流地址和解码器唯一标识的映射
    QHash<QString, QString> urlToDecoderKey_;

private slots:
    void onStreamClosed();

private:
    // 关流
    void closeStream();
};
