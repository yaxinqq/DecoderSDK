#pragma once
#include "CommonDef.h"

#include "decodersdk/common_define.h"

#include <QMap>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QThread>

#ifdef D3D11VA_AVAILABLE
#include <d3d11.h>
#include <wrl/client.h>
#endif

class StreamPipeline;
class VideoPlayerImpl;

/*!
 * \class StreamManager
 *
 * \brief 应用层的流管理器
 *
 * \note 所有会修改 Active、Retiring 或 player 映射的接口都必须在
 *       StreamManager 对象所属线程中调用，避免并发修改内部映射。
 *
 * \author ZYX
 * \date 2023-10-24
 */
class StreamManager : public QObject {
    Q_OBJECT
    Q_DISABLE_COPY(StreamManager)

public:
    static StreamManager *instance();

    /*
     * @brief 打开流（Rtsp流、文件流、ws流等，根据protocolType来定）
     *
     * @param player 流的播放器
     * @param url 流地址
     * @param openMode 打开方式 独占：一个解码器对应唯一的播放器；复用：一个解码器对应多个播放器
     * @param customConfig 自定义解码器配置项，默认为空
     * @return 生成decoder的唯一标识
     */
    QString openStream(VideoPlayerImpl *player, QString url,
                       Stream::OpenMode openMode = Stream::OpenMode::kReuse,
                       const std::optional<decoder_sdk::DecoderConfig> &customConfig = std::nullopt);

    /*
     * @brief
     * 关闭播放器对应的流。返回值仅是这个函数的执行结果，并不是最终解码器关闭的结果。可能受到打开模式的影响
     *
     * @param player 播放器
     */
    bool closeStream(VideoPlayerImpl *player);
    /*
     * @brief
     * 关闭播放器对应的补充流。返回值仅是这个函数的执行结果，并不是最终解码器关闭的结果。可能受到打开模式的影响
     *
     * @param key 解码器的唯一标识
     * @param player 播放器
     */
    bool closeSupplementaryStream(const QString &key, VideoPlayerImpl *player);

    /*
     * @brief
     * 暂停播放器对应的流。返回值仅是这个函数的执行结果，并不是最终解码器暂停的结果。可能受到打开模式的影响
     *
     * @param player 播放器
     */
    bool pause(VideoPlayerImpl *player);

    /*
     * @brief
     * 恢复播放器对应的流。返回值仅是这个函数的执行结果，并不是最终解码器恢复的结果。可能受到打开模式的影响
     *
     * @param player 播放器
     */
    bool resume(VideoPlayerImpl *player);

    /*
     * @brief 开启录像
     *
     * @param player 播放器
     * @param recorderFilePath 录像文件路径
     */
    bool startRecoding(VideoPlayerImpl *player, const QString &recorderFilePath);

    /*
     * @brief 停止录像
     *
     * @param player 播放器
     * @param recorderFilePath 录像文件路径
     */
    bool stopRecoding(VideoPlayerImpl *player, const QString &recorderFilePath);

    /*
     * @brief 是否正在录像
     *
     * @param player 播放器
     * @param recorderFilePath 录像文件路径
     */
    bool isRecoding(VideoPlayerImpl *player, const QString &recorderFilePath);

    /*
     * @brief 跳转
     *
     * @param player 播放器
     * @param pts 时间点（单位：s）
     */
    bool seek(VideoPlayerImpl *player, double pts);

    /*
     * @brief 设备倍速
     *
     * @param player 播放器
     * @param speed 倍速
     */
    bool setSpeed(VideoPlayerImpl *player, double speed);

    /**
     * @brief 设置循环模式
     * 
     * @param player 播放器
     * @param mode 模式
     * @param maxLoops 最大循环次数
     */
    bool setLoopMode(VideoPlayerImpl *player, decoder_sdk::LoopMode mode, int maxLoops = -1);
    /**
     * @brief 重置循环计数
     *
     * @param player 播放器
     */
    bool resetLoopCount(VideoPlayerImpl *player);

    /*
     * @brief 播放器和解码器换绑
     *
     * @param key 解码器的唯一标识
     * @param player 播放器
     */
    bool switchStream(const QString &key, VideoPlayerImpl *player);

    /*
     * @brief 创建视频流解码器
     *
     * @param url 流地址
     * @param openMode 流打开方式
     * @return 解码器唯一标识
     */
    QString createStream(const QString &url, Stream::OpenMode openMode);

    /*
     * @brief 流解码器是否存在
     *
     * @param key 解码器唯一标识
     * @return 是否存在
     */
    bool isStreamExist(const QString &key) const;

    /*
     * @brief 获取默认的解码器配置
     *
     * @return 解码器配置
     */
    const decoder_sdk::DecoderConfig &defaultDecoderConfig() const;

    /**
     * @brief 在当前解码器中，注册特定事件的回调函数。只会触发一次，会和本身的eventUpdated重合。
     *        目前想到的使用场景就是某些事件是异步执行后才会触发的，但可能等执行完成后，
     *        player已经和decoder解绑了，无法收到执行结果（比如录制过程中关闭，会收不到录制结束）。
     *        所以添加了此函数
     *
     * @param player 播放器
     * @param type 注册回调函数的特定事件
     * @param cb 回调函数
     * @return 是否注册成功
     */
    bool registerEventCallback(VideoPlayerImpl *player, decoder_sdk::EventType type, 
                               std::function<void(const QString &url, decoder_sdk::EventType type,
                                                  const std::shared_ptr<decoder_sdk::EventArgs> &event)>
                                   cb);

private slots:
    /*
     * @brief 响应 StreamPipeline 准备被删除的信号
     *
     * @param key worker的标识符
     * @param pipelineId pipeline的进程内唯一标识
     */
    void onWorkerAboutToDelete(const QString &key, quint64 pipelineId);

    /*
     * @brief 响应 player 即将被销毁的信号，按稳定uuid统一解绑并清理映射
     *
     * @param uuid player的唯一标识
     */
    void onPlayerAboutToDestroyed(const QString &uuid);

    /** 
     * @brief 程序即将退出，清理当前的所有流
     */
    void onAppAboutToQuit();

private:
    /*
     * @brief Active或Retiring pipeline的稳定记录
     */
    struct PipelineRecord {
        QString key;                      // Pipeline对应的源key
        quint64 pipelineId = 0;           // Pipeline的进程内唯一标识
        QPointer<StreamPipeline> pipeline; // Pipeline指针，销毁后自动置空
    };

    /*
     * @brief 根据播放器找到其对应的管线
     *
     * @param player 播放器
     */
    StreamPipeline *pipelineByPlayer(VideoPlayerImpl *player);

    /*
     * @brief 获得或创建一个管线
     *
     * @param url 管线的流地址
     * @param openMode 管线的打开方式
     * @param key 生成的Decoder对应的key
     */
    StreamPipeline *getOrCreatePipeline(const QString &url, Stream::OpenMode openMode,
                                            QString &key);

    /*
     * @brief 创建并注册一个新的Active pipeline
     *
     * @param key pipeline对应的源key
     * @return 创建成功的pipeline
     */
    StreamPipeline *createPipeline(const QString &key);

    /*
     * @brief 将已进入终态关闭的Active pipeline移入Retiring集合
     *
     * 此函数只转移所有权和映射，不等待pipeline关闭，也不删除pipeline。
     *
     * @param worker 已经开始关闭的pipeline
     */
    void retirePipeline(StreamPipeline *worker);

    /*
     * @brief 清理所有仍指向指定pipeline的player映射
     *
     * @param worker 已进入Active或Retiring状态的pipeline
     */
    void removePlayerBindingsForPipeline(StreamPipeline *worker);

    /*
     * @brief 更新指定key的Retiring阈值告警状态
     *
     * 告警采用边沿触发，只有首次超过阈值时输出warning，数量回落到阈值以内后重新武装。
     *
     * @param key pipeline对应的源key
     */
    void updateRetiringWarning(const QString &key);

    /*
     * @brief 将最早的Retiring pipeline强制删除请求排队到下一轮事件循环
     *
     * 该函数只负责排队，不执行等待或删除。实际删除由
     * forceDeleteOldestRetiringPipeline处理。
     *
     * 如需临时关闭强制回收，只注释retirePipeline中这一函数的调用即可。
     * 被注释后不会再创建新的强制回收链，但已经排入下一轮事件循环的任务仍可能
     * 完成一次当前删除并按原逻辑继续排队。不要注释函数定义，否则会产生链接错误。
     *
     * @param key pipeline对应的源key
     */
    void queueForceDeleteOldestRetiringPipeline(const QString &key);

    /*
     * @brief 强制删除指定key中最早的Retiring pipeline
     *
     * 该函数通过QMetaObject::invokeMethod按Qt::QueuedConnection方式调用，
     * 运行在StreamManager所属线程。删除过程会执行requestStop、wait和delete，
     * 并确保当前open流程已经完成新pipeline的创建和ensureOpen调用。
     *
     * @param key pipeline对应的源key
     */
    Q_INVOKABLE void forceDeleteOldestRetiringPipeline(const QString &key);

    /*
     * @brief 初始化默认的解码器配置
     */
    void initDefaultDecoderConfig();

    /**
     * @brief 清理所有流
     */
    void clearAllStreams();

private:
    explicit StreamManager(QObject *parent = nullptr);
    ~StreamManager();

    // VideoPlayer uuid和Active Pipeline之间的映射
    QMap<QString, QPointer<StreamPipeline>> mapPipelineByPlayer_;
    // Active Pipeline的SourceKey和Pipeline记录之间的映射
    QMap<QString, PipelineRecord> mapActivePipelineByKey_;
    // Retiring Pipeline的pipelineId和Pipeline记录之间的映射
    QMap<quint64, PipelineRecord> mapRetiringPipelineById_;
    // 每个SourceKey当前仍在Retiring中的pipeline数量
    QMap<QString, int> retiringCountByKey_;
    // 每个SourceKey是否已经输出过超阈值告警
    QMap<QString, bool> retiringWarningActiveByKey_;

    // 默认的解码器配置
    decoder_sdk::DecoderConfig defaultDecoderConfig_;
};
