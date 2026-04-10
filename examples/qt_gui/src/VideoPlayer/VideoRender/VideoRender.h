#ifndef VIDEORENDER_H
#define VIDEORENDER_H
#include "CommonDef.h"

#include "decodersdk/frame.h"

#include <QMutex>
#include <QOpenGLBuffer>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QScopedPointer>
#include <QSharedPointer>

#include <memory>

class VideoRender : protected QOpenGLExtraFunctions {
public:
    // 裁剪拼接的策略
    enum class ClippingStitchStrategy : uint8_t {
        kScaleToTarget = 0,     // 源区域缩放到目标区域
        kKeepSourceSizeTopLeft, // 保持源区域不变，从目标区域左上点开始绘制
    };

public:
    VideoRender();
    virtual ~VideoRender();

    /**
     * @brief
     * 初始化OpenGL上下文，编译链接shader；如果是GPU直接与OpenGL对接数据，则会分配GPU内存或注册资源
     * 注意：未加锁，不能和uninitialize在不同线程中并发执行！
     * @param frame		 视频帧
     * @param processParam 视频处理参数
     */
    void initialize(const std::shared_ptr<decoder_sdk::Frame> &frame,
                    const Stream::VideoProcessParam &processParam = Stream::VideoProcessParam());

    /**
     * @brief 释放除当前纹理、以及绘制它所必须资源的全部
     * 注意：未加锁，不能和initialize在不同线程中并发执行！
     */
    void uninitialize();

    /**
     * @brief 渲染
     * @param frame 视频帧
     * @param videoFrameParam 视频帧参数（OUT）
     */
    void render(const std::shared_ptr<decoder_sdk::Frame> &frame,
                Stream::VideoFrameParam *videoFrameParam = nullptr);

    /**
     * @brief 绘制
     */
    void draw();

    /**
     * @brief 将图像渲染到缓存帧中，外部负责释放QOpenGLFramebufferObject
     * @return 当前显示的FBO，如果没有可用的FBO则返回nullptr
     */
    QSharedPointer<QOpenGLFramebufferObject> getFrameBuffer();

    /*
     * @brief render是否有效，目前通过是否完成初始化来判断
     *
     * @return 是否有效
     */
    bool isValid() const;
    /*
     * @brief render是否初始化
     *
     * @return 是否初始化
     */
    bool isInitialized() const;

    /*
     * @brief render是否需要重建
     *
     * @return 是否需要重建。目前仅D3D11va和DXVA2有重建需求
     */
    virtual bool shouldRebuild() const;

    /**
     * @brief 得到渲染器名称
     *
     * @return 渲染器名称
     */
    virtual QString renderName() const = 0;

public:
    /**
     * @brief 设置电子放大矩形
     *
     * @param rect 电子放大区域
     */
    void setDigitalZoomRect(const QRectF &rect);
    /**
     * @brief 获得当前电子放大的矩形区域
     *
     * @param rect 电子放大区域
     */
    QRectF digitalZoomRect() const;

    /**
     * @brief 设置是否水平翻转
     *
     * @param flip 是否水平翻转
     */
    void setHorizontalFlip(bool flip);
    /**
     * @brief 是否水平翻转
     *
     * @return 是否水平翻转
     */
    bool isHorizontalFlip() const;
    /**
     * @brief 设置是否垂直翻转
     *
     * @param flip 是否垂直翻转
     */
    void setVecticalFlip(bool flip);
    /**
     * @brief 是否垂直翻转
     *
     * @return 是否垂直翻转
     */
    bool isVecticalFlip() const;
    /**
     * @brief 设置是否水平、垂直翻转
     *
     * @param hFlip 是否水平翻转
     * @param vFlip 是否垂直翻转
     */
    void setHorizontalAndVecticalFlip(bool hflip, bool vflip);
    /**
     * @brief 是否水平翻转和垂直翻转
     *
     * @return <水平翻转, 垂直翻转>
     */
    QPair<bool, bool> isHorizontalAndVecticalFlip() const;

    /**
     * @brief 设置颜色调整值
     *
     * @param brightness 亮度
     * @param contrast 对比度
     * @param saturation 饱和度
     * @param hue 色调
     */
    void setColorAdjustments(float brightness, float contrast, float saturation, float hue);

    /**
     * @brief 设置亮度
     *
     * @param brightness 亮度
     */
    void setBrightness(float brightness);
    /**
     * @brief 获取亮度
     * @return 亮度
     */
    float brightness() const;

    /**
     * @brief 设置对比度
     *
     * @param contrast 对比度
     */
    void setContrast(float contrast);
    /**
     * @brief 获取对比度
     * @return 对比度
     */
    float contrast() const;

    /**
     * @brief 设置饱和度
     *
     * @param saturation 饱和度
     */
    void setSaturation(float saturation);
    /**
     * @brief 获取饱和度
     * @return 饱和度
     */
    float saturation() const;

    /**
     * @brief 设置色调
     *
     * @param hue 色调
     */
    void setHue(float hue);
    /**
     * @brief 获取色调
     * @return 色调
     */
    float hue() const;

    /**
     * @brief 设置裁剪拼接区域
     * @param sourceRegions 源区域
     * @param targetRegions 目标区域
     */
    void setClippingStitchRegions(const QVector<QRect> &sourceRegions,
                                  const QVector<QRect> &targetRegions);
    /**
     * @brief 清除裁剪拼接区域
     */
    void clearClippingStitchRegions();
    /**
     * @brief 是否有裁剪拼接区域
     * @return 是否有裁剪拼接区域
     */
    bool hasClippingStitchRegions() const;

    /**
     * @brief 设置裁剪拼接策略
     * @param strategy 裁剪拼接策略
     */
    void setClippingStitchStrategy(ClippingStitchStrategy strategy);
    /**
     * @brief 获取裁剪拼接策略
     * @return 裁剪拼接策略
     */
    ClippingStitchStrategy clippingStitchStrategy() const;

protected:
    /**
     * @brief 初始化VBO
     * @param horizontal 是否水平镜像
     * @param vertical 是否垂直镜像
     */
    virtual bool initRenderVbo(const bool horizontal, const bool vertical) = 0;

    /**
     * @brief 初始化渲染Shader
     * @param frame 视频帧
     */
    virtual bool initRenderShader(const decoder_sdk::Frame &frame) = 0;

    /**
     * @brief 初始化渲染纹理
     * @param frame 视频帧
     */
    virtual bool initRenderTexture(const decoder_sdk::Frame &frame) = 0;

    /**
     * @brief 初始化硬件帧互操作资源
     * @param frame 视频帧
     */
    virtual bool initInteropsResource(const decoder_sdk::Frame &frame) = 0;

    /**
     * @brief 渲染视频帧，会绘制在一个FBO上
     * @param frame 视频帧
     */
    virtual bool renderFrame(const decoder_sdk::Frame &frame) = 0;

    /**
     * @brief 清理渲染资源。会在OpenGL同步后调用，可以清理本轮次渲染视频帧的相关资源。
     */
    virtual void cleanupRenderResources()
    {
    }

    /**
     * @brief 清理所有相关的资源（特定API资源 + 对应的OpenGL资源）
     */
    virtual void cleanupAllResources() = 0;

protected:
    /*
     * @brief 创建一个默认的VBO，其中的顶点坐标和纹理坐标，分离式存储
     *        前四组（x、y）是顶点坐标，后四组（x、y)是纹理坐标
     */
    void initDefaultVBO(QOpenGLBuffer &vbo, const bool horizontal, const bool vertical) const;

    /*
     * @brief OpenGL清屏
     */
    void clearGL();

protected:
    bool isIntelGpu_ = false;

private:
    // 初始化状态
    enum class InitState : uint8_t {
        kNotAttempted = 0, // 未尝试初始化
        kInitializing = 1, // 初始化中
        kInitialized = 2,  // 已初始化
        kFailed = 3        // 初始化失败
    };

    // 处理参数更新标志位
    enum ProcessParamDirtyFlag : uint32_t {
        kDirtyNone = 0,             // 无需更新
        kDirtyZoomRect = 1u << 0,   // 更新电子放大区域
        kDirtyFlip = 1u << 1,       // 更新翻转
        kDirtyBrightness = 1u << 2, // 更新亮度
        kDirtyContrast = 1u << 3,   // 更新对比度
        kDirtySaturation = 1u << 4, // 更新饱和度
        kDirtyHue = 1u << 5         // 更新色调
    };

    /**
     * @brief 标记处理参数脏
     * @param flags 处理参数脏标志位
     */
    void markProcessParamDirty(uint32_t flags);
    /**
     * @brief 消费处理参数脏标志位
     * @return 处理参数脏标志位
     */
    uint32_t consumeProcessParamDirtyFlags();

    /**
     * @brief 尝试开始初始化
     * @return 是否成功
     */
    bool tryBeginInitialize();
    /**
     * @brief 标记初始化失败
     */
    void markInitializeFailed();
    /**
     * @brief 标记初始化成功
     */
    void markInitializeSucceeded();
    /**
     * @brief 重置初始化状态
     */
    void resetInitializeState();

    /**
     * @brief 初始化FBO绘制资源
     * @param size FBO的大小
     * @retur 是否成功
     */
    bool initializeFboDrawResources(const QSize &size);

    /**
     * @brief 根据传入的视频处理参数，更新着色器
     *
     * @param processParam 视频处理参数
     * @return
     */
    bool updateVideoProcessParams(const Stream::VideoProcessParam &processParam);

    /**
     * @brief 绘制FBO到屏幕
     * @param fbo 要绘制的FBO
     */
    void drawFbo(QSharedPointer<QOpenGLFramebufferObject> fbo);

    /**
     * @brief 创建一个FBO
     * @param size FBO的大小
     * @param fmt FBO的格式
     * @return 创建的FBO指针
     */
    QSharedPointer<QOpenGLFramebufferObject> createFbo(const QSize &size,
                                                       const QOpenGLFramebufferObjectFormat &fmt);

    /**
     * @brief 把给定的rect转换到OpenGL坐标系下，并保存为vec4，便于传给着色器
     *
     * @param rect 给定的矩形区域
     * @param needTrans 是否需要转换y值
     * @return OpenGL坐标系下的vec4
     */
    QVector4D transToOpenGLUniform(const QRectF &rect, bool needTrans = true) const;

    /**
     * @brief 得到翻转参数
     *
     * @param hFlip 是否水平翻转
     * @param vFlip 是否垂直翻转
     * @return 翻转向量（x：水平、y：垂直）
     */
    QVector2D getFlipParam(bool hFlip, bool vFlip) const;

    /**
     * @brief 处理裁剪拼接过程
     *
     * @param sourceFbo 源纹理
     * @param outputFbo 目标纹理
     * @return 是否处理成功
     */
    bool applyClippingStitch(const QSharedPointer<QOpenGLFramebufferObject> &sourceFbo,
                             const QSharedPointer<QOpenGLFramebufferObject> &outputFbo);

private:
    QMutex mtx_;
    QSharedPointer<QOpenGLFramebufferObject> curFbo_;
    QSharedPointer<QOpenGLFramebufferObject> nextFbo_;

    // 用于绘制FBO到屏幕的资源
    QOpenGLShaderProgram fboDrawProgram_;
    QOpenGLBuffer fboDrawVbo_;
    std::atomic_bool fboDrawResourcesInitialized_;

    // interop互操作资源是否准备好
    std::atomic_bool interopResourceReady_;

    // 初始化状态
    std::atomic<InitState> initState_ = InitState::kNotAttempted;
    // 跳过初始化日志已输出次数（最多连续输出2次）
    std::atomic_uint32_t skipInitializeLogCount_ = 0;
    // 是否支持glFence
    bool supportsGlFence_ = false;
    // 是否强制GPU等待
    bool forceGpuFinish_ = false;

    // 视频处理参数
    Stream::VideoProcessParam processParam_;

    // 处理参数更新标志
    std::atomic_uint32_t processParamDirtyFlags_ = kDirtyNone;

    // 裁剪拼接相关
    // 锁
    mutable QMutex clippingMtx_;
    // 裁剪输入纹理
    QSharedPointer<QOpenGLFramebufferObject> clippingInputFbo_;
    // 裁剪源区域
    QVector<QRect> clippingSourceRegions_;
    // 拼接目标区域
    QVector<QRect> stitchTargetRegions_;
    // 拼接后的输出大小
    QSize stitchOutputSize_;
    // 裁剪拼接策略
    ClippingStitchStrategy clippingStitchStrategy_ = ClippingStitchStrategy::kScaleToTarget;
};

#endif // VIDEORENDER_H
