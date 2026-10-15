#pragma once
#include "decodersdk/common_define.h"
#include "decodersdk/frame.h"
#include <QObject>

namespace decoder_sdk {
    class Frame;
}

/*!
 * \class IDecodeWorker
 *
 * \brief 解码工作器接口类，仅提供取帧能力供渲染器使用
 */
class IDecodeWorker : public QObject {
    Q_OBJECT

public:
    explicit IDecodeWorker(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    virtual ~IDecodeWorker() = default;

    /**
     * @brief 尝试从解码器队列中取出一帧
     *
     * @param type 媒体类型（视频或音频）
     * @param frame 输出参数，存储取出的帧
     * @return 成功取出返回 true，否则返回 false
     */
    virtual bool tryPopFrame(decoder_sdk::MediaType type, decoder_sdk::Frame &frame) = 0;

    /**
     * @brief 查看解码器队列中的队首帧（不弹出）
     *
     * @param type 媒体类型（视频或音频）
     * @param frame 输出参数，存储队首帧的快照
     * @return 成功获取返回 true，否则返回 false
     */
    virtual bool frontFrame(decoder_sdk::MediaType type, decoder_sdk::Frame &frame) = 0;

    /**
     * @brief 获取解码器队列中的队首帧PTS（原子操作，低开销）
     *
     * @param type 媒体类型（视频或音频）
     * @return 队首帧的PTS，如果队列为空返回0.0
     */
    virtual double frontPts(decoder_sdk::MediaType type) = 0;

signals:
    /**
     * @brief 解码队列由空变为非空时发出
     *
     * 该信号由 FrameQueue 的轻量回调解发，只用于唤醒消费方/渲染线程继续取帧。
     * 消费方回调不得执行锁等待、I/O、网络、OpenGL/解码等长时间阻塞操作。
     *
     * @param mediaType 入队的媒体类型
     */
    void frameAvailable(decoder_sdk::MediaType mediaType);
};
