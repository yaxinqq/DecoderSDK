#pragma once
#include "decodersdk/decoder_sdk_def.h"

#include <QMap>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QThread>

#include <atomic>
#include <memory>

class VideoPlayerImpl;
class StreamDecoder;

/*!
 * \class DecoderThread
 *
 * \brief 用来取解码帧的线程
 *
 * \author ZYX
 * \date 2023/9/21
 */
class DecoderThread : public QThread {
    Q_OBJECT

public:
    DecoderThread(StreamDecoder *const decoder, QObject *parent = Q_NULLPTR);
    ~DecoderThread();

protected:
    void run() override;

private:
    // StreamDecoder的指针，生命周期归外部
    QPointer<StreamDecoder> pDecoder_;

    // 是否解出I帧
    bool decodeKeyFrame_ = false;
};

/*!
 * \class SafeDeleteThread
 *
 * \brief 安全删除解码器的线程，当解码器处于isOpening的状态时，需要等待解码器完全打开在删除
 *
 * \author ZYX
 * \date 2023/9/21
 */
class SafeDeleteThread : public QThread {
    Q_OBJECT

public:
    SafeDeleteThread(StreamDecoder *const decoder, QObject *parent = Q_NULLPTR);
    ~SafeDeleteThread();

protected:
    void run() override;

private:
    // StreamDecoder的指针，生命周期归外部
    QPointer<StreamDecoder> pDecoder_;
};

/*!
 * \class SafeDeleteThread
 *
 * \brief 流解码器，主要是对cvcam::streamManager的封装
 *
 * \author ZYX
 * \date 2023/9/21
 */
class StreamDecoder : public QObject {
    Q_OBJECT

    friend class DecoderThread;
    friend class SafeDeleteThread;

public:
    /*
     * @brief 解码器需要执行的任务
     *
     */
    enum class Task : uint8_t {
        kPause,  // 暂停
        kResume, // 恢复
        kClose   // 关闭
    };

public:
    explicit StreamDecoder(QObject *parent = nullptr);
    virtual ~StreamDecoder();

    // 安全删除这个Decoder，请用这个关闭StreamDecoder
    void safeDelete();

    // 异步的打开流
    void openAsync(const QString &url, const decoder_sdk::Config &config);

    // 执行任务
    void doTask(Task task);

public slots:
    // 开启录像，结果根据event变更进行处理
    void onNeedToStartRecoding(const QString &recordPath, int flags = 0);
    // 关闭录像，结果根据event变更进行处理
    void onNeedToStopRecording();

    void onNeedToSeek(double pos);

signals:
    // 录像状态变更  isRecoding - 是否正在录像
    void recordingStatusChanged(bool isRecoding);

private:
    // 暂停解码器
    int pause();
    // 恢复解码器
    int resume();
    // 关闭解码器
    int close();
    // 是否正在解码
    bool isDecoding() const;

    // 打开流后的回调函数
    void openCallback(decoder_sdk::AsyncOpenResult result, bool openSuccess,
                      const std::string &errorMessage);
    // 流事件的回调函数
    void streamEventCallback(decoder_sdk::EventType type,
                             std::shared_ptr<decoder_sdk::EventArgs> event);

    // 开始提取解码帧
    void decode();

signals:
    // 发送流打开结果
    void openResultReady(bool success, const QString &errorMsg);
    // 发送帧
    void videoFrameReady(const decoder_sdk::Frame &frame);
    // 发送销毁信号
    void aboutToDelete();
    // 发送流事件通知
    void eventUpdated(decoder_sdk::EventType type,
                      const std::shared_ptr<decoder_sdk::EventArgs> &event);

private:
    decoder_sdk::DecoderController controller_;

    std::atomic_bool isOpening_ = false; // 流正在打开中
    QMutex mutex_;                       // "完成任务"的状态锁

    std::atomic_bool isSeeking_ = false;

    // 提取解码帧的线程
    DecoderThread *decodeThread_ = nullptr;
    // 安全删除的线程
    SafeDeleteThread *safeDeleteThread_ = nullptr;
};

/*!
 * \class SafeDeleteThread
 *
 * \brief 流解码器的消息队列线程（解码器相当于消费者）
 * 注意：该类继承自QThread，只有run中的操作，才是执行在另一个线程中。其它函数均在主线程中执行
 *
 * \author ZYX
 * \date 2023/9/21
 */
class StreamDecoderWorker : public QThread {
    Q_OBJECT
public:
    StreamDecoderWorker(const QString &key, QObject *parent = nullptr);
    ~StreamDecoderWorker();

    // 往队列中加一个任务
    void appendTask(StreamDecoder::Task task);
    // 打开解码器
    void open(const QString &url, const decoder_sdk::Config &config);

    // 当前是否在录像
    bool isRecodering() const;
    // 解码器是否处于将亡状态
    bool decoderPreparingToClose() const;

    // 获得Worker的唯一标识
    QString key() const;

    // 将player和decoder绑定
    void registerPlayer(VideoPlayerImpl *player);
    // 将player和decoder解绑
    void unRegisterPlayer(VideoPlayerImpl *player);

signals:
    // 发送即将销毁的信号
    void aboutToDelete(const QString &key);
    // 发送打开流的信号
    void openAsync(const QString &url, const decoder_sdk::Config &config);
    // 发送任务
    void task(StreamDecoder::Task t);

    /*
     * @brief 开始录像
     *
     * @param recordDir 录像存储目录
     * @param flags 存储标志，参加RecordingFlag定义
     */
    void needToStartRecoding(const QString &recordDir, int flags = 0);

    /*
     * @brief 停止录像
     *
     */
    void needToStopRecording();

    void needToSeek(double pos);

protected:
    void run() override;

private:
    /*
     * @brief 判断是否应该执行此任务
     *
     * @param task 待执行的任务
     * @return 应该执行任务，返回true
     */
    bool shouldExecuteTask(StreamDecoder::Task task) const;

    /*
     * @brief 设置录像的状态
     *
     * @param status 是否在录像
     */
    void setRecordingStatus(bool status);

private:
    // 解码器
    QPointer<StreamDecoder> decoder_ = nullptr;
    // 解码器的工作线程
    QThread *thread_ = nullptr;

    // 解码器的任务队列，目前的业务场景下，只会保存最新的一条任务（如，暂停-恢复-暂停-恢复，只需要响应最后一个恢复就行）
    QQueue<StreamDecoder::Task> tasks_;
    // 保护锁
    std::mutex mutex_;
    // 条件变量，用于唤醒消费者（解码器）
    std::condition_variable condition_;

    // 标志位，只能打开一次解码器
    std::atomic_bool once_ = false;

    // 和这个解码器关联的Player，相当于智能指针的引用计数
    QList<VideoPlayerImpl *> refPlayers_;

    // 用来唯一标识Worker
    QString key_;

    // 是否正在录像
    bool isRecording_ = false;
    // 是否处于将亡状态
    bool decoderPreparingToClose_ = false;
};