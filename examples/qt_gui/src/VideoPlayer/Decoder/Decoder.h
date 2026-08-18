#pragma once
#include "IDecodeWorker.h"

#include "decodersdk/decoder_controller.h"

#include <QPointer>

#pragma region DecodeWorker
class Decoder;
class DecoderHelper;
class QThread;

/*!
 * \class DecodeWorker
 *
 * \brief Decoder + QThread的封装
 *
 * \author ZYX
 * \date 2026/06/29
 */
class DecodeWorker : public IDecodeWorker {
    Q_OBJECT

public:
    /**
     * @brief 任务类型
     */
    enum class Task : uint8_t {
        kPause,
        kResume,
        kClose
    };

public:
    DecodeWorker(const QString &key = QString(), QObject *parent = nullptr);
    ~DecodeWorker();

    /**
     * @brief 异步打开流
     *
     * @param url 流地址
     * @param config 解码器配置
     */
    void openAsync(const QString &url, const decoder_sdk::DecoderConfig &config);

    /**
     * @brief 执行任务
     *
     * @param task 任务类型
     */
    void doTask(Task task);

    /**
     * @brief 广播流信息以及解码器信息
     */
    void broadcastStreamAndDecoderInfo();

    /**
     * @brief 开启录像，结果根据event变更进行处理
     *
     * @param recordPath 录像路径
     */
    void startRecoding(const QString &recordPath);
    /**
     * @brief 关闭录像，结果根据event变更进行处理
     */
    void stopRecording();
    /**
     * @brief 进行跳转
     *
     * @param pos 跳转位置
     */
    void seek(double pos);
    /**
     * @brief 设置倍速
     *
     * @param speed 倍速
     */
    void setSpeed(double speed);

    /**
     * @brief 设置循环模式
     *
     * @param mode 循环模式
     * @param maxLoops 最大循环次数
     */
    void setLoopMode(decoder_sdk::LoopMode mode, int maxLoops = -1);
    /**
     * @brief 重置循环计数
     */
    void resetLoopCount();

    /**
     * @brief 尝试弹出一帧（线程安全）
     *
     * @param type 帧类型
     * @param frame 弹出的帧
     * @return 是否成功弹出
     */
    bool tryPopFrame(decoder_sdk::MediaType type, decoder_sdk::Frame &frame) override;

    /**
     * @brief 查看队首帧（线程安全，不弹出）
     *
     * @param type 帧类型
     * @param frame 队首帧的快照
     * @return 是否成功获取
     */
    bool frontFrame(decoder_sdk::MediaType type, decoder_sdk::Frame &frame) override;

    /**
     * @brief 获取队首帧PTS（原子操作，低开销）
     *
     * @param type 帧类型
     * @return 队首帧的PTS
     */
    double frontPts(decoder_sdk::MediaType type) override;

signals:
    /**
     * @brief 发送流打开结果
     *
     * @param success 是否成功打开
     * @param errorMsg 错误信息
     */
    void openResultReady(bool success, const QString &errorMsg);
    /**
     * @brief 发送请求销毁信号，由外部进行销毁
     */
    void requestToDelete();
    /**
     * @brief 发送流事件通知
     *
     * @param url 流地址
     * @param type 事件类型
     * @param event 事件参数
     */
    void eventUpdated(const QString &url, decoder_sdk::EventType type, const std::shared_ptr<decoder_sdk::EventArgs> &event);

    /**
     * @brief 发送流信息变更
     *
     * @param info 流信息
     */
    void streamInfoUpdated(const std::optional<decoder_sdk::StreamInfo> &info);
    /**
     * @brief 发送流统计信息变更（如实时视频码率）
     *
     * @param info 流统计信息
     */
    void streamStaticsInfoUpdated(const decoder_sdk::StreamStaticsInfo &info);
    /**
     * @brief 发送解码器信息变更
     *
     * @param mediaType 媒体类型
     * @param info 解码器信息
     */
    void decoderInfoUpdated(decoder_sdk::MediaType mediaType, const std::optional<decoder_sdk::DecoderInfo> &info);

private:
    // 解码器，借助QPointer来获取指针的有效性
    QPointer<Decoder> decoder_;
    // 解码器所在线程
    QThread *thread_ = nullptr;

    // 辅助通信
    DecoderHelper *helper_ = nullptr;
};
#pragma endregion