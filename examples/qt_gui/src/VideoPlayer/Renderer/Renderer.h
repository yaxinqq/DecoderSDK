#pragma once
#include "../Base/CommonDef.h"

#include <QObject>
#include <QPointer>

#include <memory>
#include <mutex>

#pragma region RenderWorker
class IDecodeWorker;
class DisplayRenderer;
class QSurface;
class QThread;
class QOpenGLContext;
class Renderer;
class RendererHelper;
/*!
 * \class RenderWorker
 *
 * \brief Renderer + QThread的封装
 *
 * \author ZYX
 * \date 2026/06/29
 */
class RenderWorker : public QObject {
    Q_OBJECT

public:
    // surface由主线程创建、也由主线程销毁
    RenderWorker(QSurface *surface, const QString &key = QString(), QObject *parent = nullptr);
    ~RenderWorker();

    /**
     * @brief 开启渲染
     */
    void start();
    /**
     * @brief 停止渲染
     */
    void stop();

    /**
     * @brief 重置时间轴（通常在跳转调用）
     */
    void resetTimeline();
    /**
     * @brief 设置播放倍速
     * @param speed 倍速
     */
    void setSpeed(double speed);
    /**
     * @brief 设置主时钟类型
     * @param type 时钟类型
     */
    void setClockSourceType(Stream::ClockSourceType type);

    /**
     * @brief 设置音量 (0.0 - 1.0)
     */
    void setVolume(qreal volume);
    /**
     * @brief 获取音量
     */
    qreal volume() const;

public:
    /**
     * @brief 设置当前渲染器对应的解码器
     *
     * @param decoder 解码器
     */
    void setDecoder(QPointer<IDecodeWorker> decoder);

    /**
     * @brief 添加显示渲染器
     *
     * @param playerId 玩家ID
     */
    void addDisplayRenderer(const QString &playerId);
    /**
     * @brief 移除显示渲染器
     *
     * @param playerId 玩家ID
     */
    void removeDisplayRenderer(const QString &playerId);

signals:
    /**
     * @brief 渲染器名称改变信号
     *
     * @param rendererName 渲染器名称
     */
    void renderNameChanged(const QString &rendererName);
    /**
     * @brief 视频纹理准备就绪信号
     *
     * @param videoFrameParam 视频帧参数
     */
    void textureReady(const Stream::VideoFrameParam &videoFrameParam);

    /**
     * @brief 显示渲染器准备就绪信号
     *
     * @param playerId 玩家ID
     * @param renderer 显示渲染器
     */
    void displayRendererReady(const QString &playerId, std::weak_ptr<DisplayRenderer> renderer);
    /**
     * @brief 显示渲染器准备销毁信号
     *
     * @param playerId 玩家ID
     */
    void displayRendererAboutToDestroy(const QString &playerId);

private:
    /**
     * @brief 初始化，会在此函数中开启线程以及创建的Renderer
     */
    void initialize();

private:
    // 渲染器，借助QPointer来获取指针的有效性
    QPointer<Renderer> renderer_;
    // 渲染器所在线程
    QThread *thread_ = nullptr;

    // 辅助通信的类
    RendererHelper *helper_ = nullptr;

    // 外部传入的渲染表面
    QSurface *surface_ = nullptr;
    // 外部传入的对应的解码器
    QPointer<IDecodeWorker> decoder_;
    // 对应的流标识
    QString key_;

    // 播放状态缓存（用于懒赋值）
    double speed_ = 1.0;
    Stream::ClockSourceType clockSourceType_ = Stream::ClockSourceType::kAudioMaster;
    qreal volume_ = 1.0;

    // 初始化标志
    std::once_flag initFlag_;
};
#pragma endregion