#pragma once
#ifdef VULKAN_AVAILABLE

#include "CommonUtils.h"
#include "VideoRender.h"

#include <QMutex>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>

class Nv12Render_Vulkan : public VideoRender {
public:
    Nv12Render_Vulkan();
    ~Nv12Render_Vulkan() override;

protected:
    /**
     * @brief 初始化VBO
     * @param horizontal 是否水平镜像
     * @param vertical 是否垂直镜像
     */
    bool initRenderVbo(const bool horizontal, const bool vertical) override;

    /**
     * @brief 初始化渲染Shader
     * @param frame 视频帧
     */
    bool initRenderShader(const decoder_sdk::Frame &frame) override;

    /**
     * @brief 初始化渲染纹理
     * @param frame 视频帧
     */
    bool initRenderTexture(const decoder_sdk::Frame &frame) override;

    /**
     * @brief 初始化硬件帧互操作资源
     * @param frame 视频帧
     */
    bool initInteropsResource(const decoder_sdk::Frame &frame) override;

    /**
     * @brief 渲染视频帧，会绘制在一个FBO上
     * @param frame 视频帧
     */
    bool renderFrame(const decoder_sdk::Frame &frame) override;

private:
    /*
     * @brief 绘制视频帧
     */
    void drawFrame(GLuint idY, GLuint idUV);

    /**
     * @brief 准备导出到外部的数据集
     * 
     * @param w 帧的宽度
     * @param h 帧的高度
     * @param sizeY Y平面分量的大小
     * @param sizeUV UV屏幕分量的大小
     * @return 是否准备成功
     */
    bool prepareExternalBuffer(int w, int h, uint64_t &sizeY, uint64_t &sizeUV);

    /**
     * @brief 查找合适的内存类型
     * 
     * @param typeBits 该内存类型可用于资源所要求的类型掩码
     * @param props 需要的属性
     * @return 适合的内存类型索引
     */
    uint32_t findMemoryTypeLocal(uint32_t typeBits, VkMemoryPropertyFlags props);

    /**
     * @brief 初始化导出信号量
     * 
     * @return 是否成功
     */
    bool initExternalSemaphores();

    /**
     * @brief semReady_的handle
     * 
     * @return handle指针
     */
    void *readySemaphoreHandle() const;
    /**
     * @brief semComplete_的handle
     *
     * @return handle指针
     */
    void *completeSemaphoreHandle() const;
    /**
     * @brief externalBuffer的handle
     *
     * @return handle指针
     */
    void *externalBufferHandle() const;

    /**
     * @brief 关闭所有外部信号
     */
    void shutdownExternalSemaphores();

    /**
     * @brief 拷贝图像
     * 
     * @param vulkanFrame vulkan帧
     * @param w 宽度
     * @param h 高度
     * @return 是否成功
     */
    bool copyImageToExternalBuffer(const std::shared_ptr<decoder_sdk::VulkanFrame> &vulkanFrame, int w, int h);

    /**
     * @brief 创建命令池
     * 
     * @return 是否创建成功
     */
    bool createCommandPool();

private:
    // vulkan的相关对象
    const vkb::Instance &vkInstance_;
    const vkb::PhysicalDevice &vkPhysicalDevice_;
    const vkb::Device &vkDevice_;
    const vkb::InstanceDispatchTable &vkInstanceDispatchTable_;
    const vkb::DispatchTable &vkDispatchTable_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;

    VkBuffer extBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory extMemory_ = VK_NULL_HANDLE;
    uint64_t extBufferSize_ = 0;
    uint64_t extOffsetY_ = 0;
    uint64_t extOffsetUV_ = 0;

    VkSemaphore semReady_ = VK_NULL_HANDLE;
    VkSemaphore semComplete_ = VK_NULL_HANDLE;
    bool semInitialized_ = false;

    uint32_t graphicsQueueIndex_ = 0;
    VkQueue graphocsQueue_ = VK_NULL_HANDLE;


    // OpenGL的相关对象
    QOpenGLShaderProgram program_;
    QOpenGLBuffer vbo_;

    GLuint memObj_ = 0;
    GLuint pbo_ = 0;
    size_t memSize_ = 0;
    GLuint glReadySem_ = 0;
    GLuint glCompleteSem_ = 0;
};

#endif