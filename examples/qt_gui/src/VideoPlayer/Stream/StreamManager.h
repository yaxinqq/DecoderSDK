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
                       Stream::OpenMode openMode = Stream::OpenMode::kExclusive,
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
     * @param recodDir 保存录像的目录
     */
    bool startRecoding(VideoPlayerImpl *player, const QString &recodDir);

    /*
     * @brief 停止录像
     *
     * @param player 播放器
     */
    bool stopRecoding(VideoPlayerImpl *player);

    /*
     * @brief 是否正在录像
     *
     * @param player 播放器
     */
    bool isRecoding(VideoPlayerImpl *player);

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
     * @brief 响应 StreamDecoderWorker准备被删除 的信号
     *
     * @param key worker的标识符
     */
    void onWorkerAboutToDelete(const QString &key);

    /** 
     * @brief 程序即将退出，清理当前的所有流
     */
    void onAppAboutToQuit();

private:
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

    // VideoPlayer和Pipeline之间的映射
    QMap<VideoPlayerImpl *, QPointer<StreamPipeline>> mapPipelineByPlayer_;
    // Pipeline的SourceKey和Pipeline之间的映射
    QMap<QString, StreamPipeline *> mapPipelineByKey_;

    // 默认的解码器配置
    decoder_sdk::DecoderConfig defaultDecoderConfig_;
};