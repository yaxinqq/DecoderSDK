#pragma once
#include "Decoder.h"
#include "Renderer.h"
#include "../Base/CommonDef.h"

#include "decodersdk/decoder_sdk_def.h"

#include <QMap>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QThread>

#include <atomic>
#include <memory>

class StreamRenderer;
class VideoPlayerImpl;

/*!
 * \class StreamPipeline
 *
 * \brief 流管线，用来管理多媒体流的解码、渲染等完整的生命周期
 */
class StreamPipeline : public QThread {
    Q_OBJECT
public:
    StreamPipeline(const QString &key, QObject *parent = nullptr);
    ~StreamPipeline();

    // 往队列中加一个任务
    void appendTask(DecodeWorker::Task task);
    // 打开解码器
    void open(const QString &url, const decoder_sdk::DecoderConfig &config);

    // 当前是否在录像
    bool isRecodering() const;
    // 解码器是否处于将亡状态
    bool decoderPreparingToClose() const;

    // 获得Worker的唯一标识
    QString key() const;

    // 将player和Stream绑定
    void registerPlayer(VideoPlayerImpl *player);
    // 将player和Stream解绑
    void unRegisterPlayer(VideoPlayerImpl *player);

    // 注册特定的事件回调函数
    bool registerEventCallback(decoder_sdk::EventType type,
                               std::function<void(const QString &url, decoder_sdk::EventType type,
                                                  const std::shared_ptr<decoder_sdk::EventArgs> &event)>
                                   cb);

signals:
    // 向外部发送即将销毁的信号
    void aboutToDelete(const QString &key);

    // ================= 解码器发出，向Player分发 ================= //
    // 向外部发送流事件通知
    void eventUpdated(const QString &url, decoder_sdk::EventType type,
                      const std::shared_ptr<decoder_sdk::EventArgs> &event);
    // 向外部发送流信息变更
    void streamInfoUpdated(const std::optional<decoder_sdk::StreamInfo> &info);
    // 向外部发送解码器信息变更
    void decoderInfoUpdated(decoder_sdk::MediaType mediaType, const std::optional<decoder_sdk::DecoderInfo> &info);

    // ================= 需要解码器进行操作的信号 ================= //
    // 需要解码器异步开流
    void requestToOpenAsync(const QString &url, const decoder_sdk::DecoderConfig &config);
    // 需要解码器进行任务操作
    void requestToDoTask(DecodeWorker::Task t);
    // 需要解码器广播流信息以及解码器信息
    void requestToBroadcastStreamAndDecoderInfo();
    // 需要解码器开始录像
    void requestToStartRecoding(const QString &recordDir);
    // 需要解码器停止录像
    void requestToStopRecording();
    // 需要解码器进行Seek
    void requestToSeek(double pos);
    // 需要解码器变速
    void requestToSetSpeed(double speed);

    // 需要解码器设置循环播放模式
    void requestToSetLoopMode(decoder_sdk::LoopMode mode, int maxLoops = -1);
    // 需要解码器重置循环计数
    void requestToResetLoopCount();
    // ============================================================ //

    // ================= 渲染器发出，向Player分发 ================= //
    // 渲染器名称发生变化
    void renderNameChanged(const QString &rendererName);
    // 一帧已准备好
    void textureReady(const Stream::VideoFrameParam &videoFrameParam);
    // 为player创建的展示渲染器已准备好
    void displayRendererReady(const QString &playerId, std::weak_ptr<DisplayRenderer> renderer);
    // 展示渲染器将被销毁
    void displayRendererAboutToDestroy(const QString &playerId);
    // ============================================================ //

protected:
    void run() override;

private slots:
    /**
     * @brief 解码器异步开启结果已就绪
     *
     * @param res 异步开启成功还是失败
     * @param errorMsg 如果开启失败，错误信息
     */
    void onOpenResultReady(bool res, const QString &errorMsg);

    /**
     * @brief 解码器请求销毁
     */
    void onDecoderRequestToDelete();

    /**
     * @brief 响应解码器事件变更，并在这里进行分发
     *
     * @param url decoder对应的url
     * @param type 事件类型
     * @param event 事件
     */
    void onEventUpdated(const QString &url, decoder_sdk::EventType type,
                        const std::shared_ptr<decoder_sdk::EventArgs> &event);

private:
    void initDecoder(const QString &key);
    void initRenderer(const QString &key);

    /*
     * @brief 判断是否应该执行此任务
     *
     * @param task 待执行的任务
     * @return 应该执行任务，返回true
     */
    bool shouldExecuteTask(DecodeWorker::Task task) const;
    /*
     * @brief 设置录像的状态
     *
     * @param status 是否在录像
     */
    void setRecordingStatus(bool status);

private:
    // 解码器
    QPointer<DecodeWorker> decoder_;
    // 渲染器
    QPointer<RenderWorker> renderer_;

    // 解码器的任务队列，目前的业务场景下，只会保存最新的一条任务（如，暂停-恢复-暂停-恢复，只需要响应最后一个恢复就行）
    QQueue<DecodeWorker::Task> tasks_;
    // 保护锁
    std::mutex mutex_;
    // 条件变量，用于唤醒消费者（解码器）
    std::condition_variable condition_;

    // 标志位，只能打开一次解码器
    std::atomic_bool once_ = false;

    // 和这个流关联的Player，相当于智能指针的引用计数
    QList<VideoPlayerImpl *> refPlayers_;

    // 用来唯一标识流
    QString key_;

    // 是否正在录像
    bool isRecording_ = false;
    // 是否处于将亡状态
    bool decoderPreparingToClose_ = false;

    // 保存特定事件的回调函数
    QMap<decoder_sdk::EventType,
         QList<std::function<void(const QString &url, decoder_sdk::EventType type,
                                  const std::shared_ptr<decoder_sdk::EventArgs> &event)>>>
        eventCallbackMap_;
};
