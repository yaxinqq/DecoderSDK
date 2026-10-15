#pragma once

#include "../Player/VideoPlayer.h"
#include "../Base/CommonDef.h"

class FileStreamPlayer : public VideoPlayer {
    Q_OBJECT

public:
    explicit FileStreamPlayer(QWidget *parent = nullptr);
    virtual ~FileStreamPlayer();

public:
    /**
     * @brief 获取视频流地址
     *
     * @return 视频流地址
     */
    QString streamUrl() const;

    /**
     * @brief 打开视频流
     *
     * @param url 视频流地址
     */
    virtual void open(const QString &url);
    /**
     * @brief 关闭视频流
     *
     */
    virtual void close();
    /**
     * @brief 暂停播放视频
     *
     */
    virtual void pause() override;
    /**
     * @brief 恢复播放视频
     *
     */
    virtual void resume() override;

    /**
     * @brief 跳转到对应的时间点
     *
     * @param pts 时间点，单位s
     * @return 是否成功跳转
     */
    virtual bool seekTo(double pts);
    /**
     * @brief 跳转到对应的时间点
     *
     * @param speed 速度值
     * @return 是否成功设置速度
     */
    virtual bool setSpeed(double speed);

    /**
     * @brief 开启录像
     *
     * @param recorderFilePath 录像文件的路径
     */
    virtual void startRecoding(const QString &recorderFilePath) override;

    /**
     * @brief 停止录像
     *
     * @param recorderFilePath 录像文件的路径
     */
    virtual void stopRecoding(const QString &recorderFilePath) override;
    /**
     * @brief 是否正在录像
     *
     * @param recorderFilePath 录像文件的路径
     * @return 是否正在录像
     */
    virtual bool isRecording(const QString &recorderFilePath) const override;

private:
    QString streamUrl_;
};
