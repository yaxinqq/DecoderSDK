#pragma once
#ifdef VULKAN_AVAILABLE

#include "CommonUtils.h"
#include "VideoRender.h"

#include <QMutex>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>

class Nv12Render_Vulkan : public VideoRender {
public:
    Nv12Render_Vulkan();
    ~Nv12Render_Vulkan() override;

    /**
     * @brief 得到渲染器名称
     *
     * @return 渲染器名称
     */
    QString renderName() const override;

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
    /**
     * @brief 初始化绘制管线
     *
     * @param width 帧宽度
     * @param height 帧高度
     * @return 是否初始化成功
     */
    bool initGraphicsPipeline(uint32_t width, uint32_t height);

    // Vulkan和OpenGL互操作资源初始化
    /**
     * @brief Vulkan和OpenGL互操作资源初始化
     *
     * @param width 帧宽度
     * @param height 帧高度
     * @return 是否初始化成功
     */
    bool initInteropResources(uint32_t width, uint32_t height);

    // 执行YUV到RGBA的转换
    /**
     * @brief NV12 VkImage 转换到 RGBA VkImage
     *
     * @param frame 视频帧
     * @return 是否转换成功
     */
    bool convertNV12ToRGBA(const decoder_sdk::Frame &frame);

    /**
     * @brief 确保用来互操作的可读取FBO已初始化完成
     *
     * @return 是否初始化成功
     */
    bool ensureInteropReadFbo();

    /**
     * @brief 拷贝当前的FBO
     *
     * @param width 纹理宽度
     * @param height 纹理高度
     * @return 拷贝是否完成
     */
    bool blitToCurrentFbo(int width, int height);

#ifdef _WIN32
    /*
     * @brief 导出共享内存句柄
     *
     * @param memory vulkan侧的共享内存
     * @param outHandle 句柄（OUT）
     * @return 是否导出成功
     */
    bool exportMemoryHandle(VkDeviceMemory memory, HANDLE &outHandle);
#else
    /*
     * @brief 导出共享内存描述符
     *
     * @param memory vulkan侧的共享内存
     * @param outFd 描述符（OUT）
     * @return 是否导出成功
     */
    bool exportMemoryHandle(VkDeviceMemory memory, int &outFd);
#endif

#ifdef _WIN32
    /*
     * @brief 导出共享信号量句柄
     *
     * @param semaphore vulkan侧的共享信号量
     * @param outHandle 句柄（OUT）
     * @return 是否导出成功
     */
    bool exportSemaphoreHandle(VkSemaphore semaphore, HANDLE &outHandle);
#else
    /*
     * @brief 导出共享信号量描述符
     *
     * @param semaphore vulkan侧的共享信号量
     * @param outFd 描述符（OUT）
     * @return 是否导出成功
     */
    bool exportSemaphoreHandle(VkSemaphore semaphore, int &outFd);
#endif

    /**
     * @brief 找到对应导出内存的格式
     *
     * @param typeFilter 格式过滤器
     * @param properties vulkan支持的内存属性
     * @return 内存格式
     */
    uint32_t findMemoryTypeIndex(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    /**
     * @brief 清理Vulkan资源
     */
    void cleanupVulkanResources();
    /**
     * @brief 清理OpenGL资源
     */
    void cleanupOpenGLResources();

private:
    // vulkan的相关对象
    const vkb::Instance &vkInstance_;
    const vkb::PhysicalDevice &vkPhysicalDevice_;
    const vkb::Device &vkDevice_;
    const vkb::InstanceDispatchTable &vkInstanceDispatchTable_;
    const vkb::DispatchTable &vkDispatchTable_;

    // Vulkan渲染管线
    VkPipeline graphicsPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout graphicsPipelineLayout_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;

    // Vulkan采样器
    VkSamplerYcbcrConversion ycbcrConversion_ = VK_NULL_HANDLE;
    VkSampler ycbcrSampler_ = VK_NULL_HANDLE;

    // Vulkan RGBA输出纹理（用于导出给OpenGL）
    VkImage rgbaImage_ = VK_NULL_HANDLE;
    VkDeviceMemory rgbaMemory_ = VK_NULL_HANDLE;
    VkImageView rgbaImageView_ = VK_NULL_HANDLE;

    // Vulkan 描述符（用于渲染管线绑定资源）
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;

    // Vulkan 图形队列
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    uint32_t graphicsQueueIndex_ = 0;

    // OpenGL导入的RGBA纹理
    GLuint glRGBATexture_ = 0;
    GLuint glMemoryObject_ = 0;
    GLuint glInteropReadFbo_ = 0;
    bool horizontalMirror_ = false;
    bool verticalMirror_ = false;

    // 标记是否初始化成功
    bool isInteropInitialized_ = false;
};

#endif
