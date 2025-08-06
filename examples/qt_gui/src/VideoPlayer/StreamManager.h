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

class VideoPlayerImpl;
class StreamDecoderWorker;

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
     * @param devId 设备Id
     * @param openMode 打开方式 独占：一个解码器对应唯一的播放器；复用：一个解码器对应多个播放器
     * @return 生成decoder的唯一标识
     */
    QString openStream(VideoPlayerImpl *player, QString url, QString devId,
                       Stream::OpenMode openMode = Stream::OpenMode::kExclusive);

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

    bool seek(VideoPlayerImpl *player, double pts);

    bool setSpeed(VideoPlayerImpl *player, double speed);

    /*
     * @brief 获得默认录像文件名称
     *
     * @param player 播放器
     */
    QString defaultRecordFileName(VideoPlayerImpl *player);

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

private slots:
    /*
     * @brief 响应 StreamDecoderWorker准备被删除 的信号
     *
     * @param key worker的标识符
     */
    void onWorkerAboutToDelete(const QString &key);

private:
    /*
     * @brief 根据播放器找到其对应的解码器
     *
     * @param player 播放器
     */
    StreamDecoderWorker *streamDecoderByPlayer(VideoPlayerImpl *player);

    /*
     * @brief 获得或创建一个解码器
     *
     * @param url 解码器的流地址
     * @param openMode 解码器的打开方式
     * @param key 生成的Decoder对应的key
     */
    StreamDecoderWorker *getOrCreateDecoder(const QString &url, Stream::OpenMode openMode,
                                            QString &key);

    /*
     * @brief 创建硬件上下文的回调
     *
     * @param type 硬件类型
     */
    void *createHwContextCallback(decoder_sdk::HWAccelType type);
    /*
     * @brief 销毁硬件上下文的回调
     *
     * @param type 硬件类型
     * @param userHwContext 用户创建的硬件上下文
     */
    void freeHwContextCallback(decoder_sdk::HWAccelType type, void *userHwContext);

private:
    explicit StreamManager(QObject *parent = nullptr);
    ~StreamManager();

    // VideoPlayer和Decoder之间的映射
    QMap<VideoPlayerImpl *, QPointer<StreamDecoderWorker>> mapDecoderByWidget_;
    // Decoder的SourceKey和Decoder之间的映射
    QMap<QString, StreamDecoderWorker *> mapDecoderByKey_;
};