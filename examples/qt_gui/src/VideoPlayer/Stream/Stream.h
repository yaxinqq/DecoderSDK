#pragma once
#include "Decoder.h"
#include "Renderer.h"

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

    /*
     * @brief 往队列中加一个任务
     *
     * @param task 待执行的任务
     * @return 任务被接受时返回true，管线正在关闭或任务无法执行时返回false
     */
    bool appendTask(DecodeWorker::Task task);
    /*
     * @brief 确保解码器会话已打开
     *
     * 该操作是幂等的：未打开或打开失败时发起异步打开，正在打开时复用当前请求，
     * 已打开时恢复播放，正在关闭时拒绝复用。
     *
     * @param url 流地址
     * @param config 首次打开或重试使用的解码器配置
     * @return 请求被接受或已打开的会话可以直接复用时返回true，管线正在关闭时返回false
     */
    bool ensureOpen(const QString &url, const decoder_sdk::DecoderConfig &config);
    // 请求停止任务循环，用于退出清理等场景
    void requestStop();

    // 当前是否在录像
    bool isRecodering(const QString &recorderFilePath) const;
    /*
     * @brief 当前管线是否已经接受终态关闭任务
     *
     * @return 已接受关闭任务时返回true
     */
    bool isClosing() const;

    // 获得Worker的唯一标识
    QString key() const;

    /*
     * @brief 获得当前管线的进程内全局唯一标识
     *
     * 该标识用于在相同 key 的 Active 和 Retiring pipeline 之间精确区分实例。
     *
     * @return 进程生命周期内唯一且单调递增的 pipeline ID
     */
    quint64 pipelineId() const;

    // 将player和Stream绑定
    void registerPlayer(VideoPlayerImpl *player);
    // 将player和Stream解绑
    void unRegisterPlayer(VideoPlayerImpl *player);
    // 根据稳定的player id解绑
    void unRegisterPlayerById(const QString &playerId);
    // 当前管线是否还绑定了指定player
    bool hasPlayer(const QString &playerId) const;

    // 注册特定的事件回调函数
    bool registerEventCallback(decoder_sdk::EventType type,
                               std::function<void(const QString &url, decoder_sdk::EventType type,
                                                  const std::shared_ptr<decoder_sdk::EventArgs> &event)>
                                   cb);

signals:
    /*
     * @brief 向外部发送即将销毁的信号
     *
     * pipelineId 用于区分复用同一 key 的新旧实例，避免旧实例的延迟通知
     * 清除新实例的 Active 映射。
     *
     * @param key pipeline对应的源key
     * @param pipelineId pipeline的进程内唯一标识
     */
    void aboutToDelete(const QString &key, quint64 pipelineId);

    // ================= 解码器发出，向Player分发 ================= //
    // 向外部发送流事件通知
    void eventUpdated(const QString &url, decoder_sdk::EventType type,
                      const std::shared_ptr<decoder_sdk::EventArgs> &event);
    // 向外部发送流信息变更
    void streamInfoUpdated(const std::optional<decoder_sdk::StreamInfo> &info);
    // 向外部发送流统计信息变更（如实时视频码率）
    void streamStaticsInfoUpdated(const decoder_sdk::StreamStaticsInfo &info);
    // 向外部发送解码器信息变更
    void decoderInfoUpdated(decoder_sdk::MediaType mediaType, const std::optional<decoder_sdk::DecoderInfo> &info);

    // ================= 需要解码器进行操作的信号 ================= //
    // 需要解码器异步开流
    void requestToOpenAsync(const QString &url, const decoder_sdk::DecoderConfig &config);
    // 需要解码器进行任务操作
    void requestToDoTask(DecodeWorker::Task t, quint64 sequence);
    // 需要解码器广播流信息以及解码器信息
    void requestToBroadcastStreamAndDecoderInfo();
    // 需要解码器开始录像
    void requestToStartRecoding(const QString &recorderFilePath);
    // 需要解码器停止录像
    void requestToStopRecording(const QString &recorderFilePath);
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
    /*
     * @brief 当前管线持有的解码器会话状态
     */
    enum class OpenState: uint8_t {
        kNotOpened,
        kOpening,
        kOpened,
        kFailed,
        kClosing
    };

    void initDecoder(const QString &key);
    void initRenderer(const QString &key);

    /*
     * @brief 判断是否应该执行此任务
     *
     * @param task 待执行的任务
     * @return 应该执行任务，返回true
     */
    bool shouldExecuteTask(DecodeWorker::Task task) const;

    /**
     * @brief 尝试降级任务，如果降级成功，会修改入参
     * 
     * @param task 待执行的任务（IN & OUT，可能会被修改）
     * @return 是否降级成功
     */
    bool tryDowngradeTask(DecodeWorker::Task &task) const;

private:
    // 解码器
    QPointer<DecodeWorker> decoder_;
    // 渲染器
    QPointer<RenderWorker> renderer_;

    // 解码器的任务队列，目前的业务场景下，只会保存最新的一条任务（如，暂停-恢复-暂停-恢复，只需要响应最后一个恢复就行）
    struct TaskEntry {
        DecodeWorker::Task task;
        quint64 sequence = 0;
    };
    QQueue<TaskEntry> tasks_;
    // 保护锁
    std::mutex mutex_;
    // 条件变量，用于唤醒消费者（解码器）
    std::condition_variable condition_;

    // 最新的已接受任务序号，decoder线程用它忽略已经过期的非阻塞任务
    std::shared_ptr<std::atomic<quint64>> taskSequence_;
    // 本地递增序号，只在 mutex_ 内使用
    quint64 nextTaskSequence_ = 0;

    // 解码器会话状态，供管理器线程和异步打开回调共同读取
    std::atomic<OpenState> openState_{ OpenState::kNotOpened };

    // 和这个流关联的Player，以稳定的uuid为key；QPointer防止析构后留下裸指针
    QMap<QString, QPointer<VideoPlayerImpl>> refPlayers_;

    // 用来唯一标识流
    QString key_;

    // 当前管线的进程内唯一标识，构造后不再变化
    const quint64 pipelineId_;

    // 保存特定事件的回调函数
    QMap<decoder_sdk::EventType,
         QList<std::function<void(const QString &url, decoder_sdk::EventType type,
                                  const std::shared_ptr<decoder_sdk::EventArgs> &event)>>>
        eventCallbackMap_;
};
