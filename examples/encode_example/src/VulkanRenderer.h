#pragma once

#include "OsdTextRendererStb.h"

#include "decodersdk/frame.h"

#include <vk_mem_alloc.h>

#include <VkBootstrap.h>

#include <GLFW/glfw3.h>

#include <cuda.h>

#if defined(_WIN32)
#include <Windows.h>
#else
using HANDLE = void *;
#endif

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/**
 * @brief Vulkan 渲染器：负责创建 Vulkan 实例/设备/交换链，并将解码帧显示到窗口。
 *
 * 渲染内容：
 * - 视频：一张 RGBA 纹理，全屏绘制。
 * - OSD：一张 R8 alpha 纹理，叠加绘制到右上角（启用 alpha blending）。
 *
 */
class VulkanRenderer {
public:
    VulkanRenderer();
    ~VulkanRenderer();

    /**
     * @brief 初始化 GLFW 窗口与 Vulkan。
     * @param width 窗口宽。
     * @param height 窗口高。
     * @param title 窗口标题。
     */
    bool initialize(uint32_t width, uint32_t height, const char *title);

    /**
     * @brief 释放全部资源。
     */
    void shutdown();

    /**
     * @brief GLFW 窗口是否收到关闭请求。
     */
    bool shouldClose() const;

    /**
     * @brief 处理窗口事件。
     */
    void pollEvents();

    /**
     * @brief 设置CUDA上下文，和解码器共享
     *
     * @param ctx CUDA上下文
     */
    void setCudaContext(CUcontext ctx);
    /**
     * @brief 提交CUDA硬解码类型的视频帧
     *
     * @param frame 视频帧
     */
    void submitCudaNv12Frame(const decoder_sdk::Frame &frame);

    /**
     * @brief 渲染一帧。
     * @param osdTextUtf8 需要叠加显示在右上角的时间戳（UTF-8）。
     */
    void drawFrame(const std::string &osdTextUtf8);

    /**
     * @brief 确保编码资源可用
     *
     * @param frame 编码器的可写frame，其data[0]为cuda device ptr
     * @return 是否初始化成功
     */
    bool ensureEncodedResources(const decoder_sdk::Frame &frame);
    /**
     * @brief 将当前离屏渲染帧通过CUDA interop导出到encoder frame
     *
     * @param frame 编码器的可写frame，其data[0]为cuda device ptr
     * @return 是否填充成功
     */
    bool fillEncodedFrame(decoder_sdk::Frame &frame);

    /**
     * @brief 设置离屏渲染的目标尺寸（用于编码输出尺寸匹配）
     *
     * @param width 目标宽度
     * @param height 目标高度
     */
    void setOffscreenSize(uint32_t width, uint32_t height);

private:
    // 视频的顶点数据（位置 + 纹理）
    struct VideoVertex {
        float pos[2];
        float uv[2];
    };

    // OSD图层的顶点数据（位置 + 纹理 + 颜色）
    struct OsdVertex {
        float posPx[2];
        float uv[2];
        float color[4];
    };

private:
    /**
     * @brief 初始化窗口
     *
     * @param width 宽度
     * @param height 高度
     * @param title 标题
     * @return 是否初始化成功
     */
    bool initWindow(uint32_t width, uint32_t height, const char *title);
    /**
     * @brief 初始化Vulkan
     *
     * @return 是否初始化成功
     */
    bool initVulkan();
    /**
     * @brief 创建vulkan实例
     *
     * @return 是否创建成功
     */
    bool createInstance();
    /**
     * @brief 创建vulkan渲染表面
     *
     * @return 是否创建成功
     */
    bool createSurface();
    /**
     * @brief 选择一个物理设备
     *
     * @return 是否选择成功
     */
    bool pickPhysicalDevice();
    /**
     * @brief 创建逻辑设备
     *
     * @return 是否创建成功
     */
    bool createDevice();
    /**
     * @brief 创建分配器
     *
     * @return 是否创建成功
     */
    bool createAllocator();
    /**
     * @brief 创建交换链
     *
     * @return 是否创建成功
     */
    bool createSwapchain();
    /**
     * @brief 创建渲染通道
     *
     * @return 是否创建成功
     */
    bool createRenderPass();
    /**
     * @brief 创建帧缓冲
     *
     * @return 是否创建成功
     */
    bool createFramebuffers();
    /**
     * @brief 创建命令池
     *
     * @return 是否创建成功
     */
    bool createCommandPool();
    /**
     * @brief 创建同步对象
     *
     * @return 是否创建成功
     */
    bool createSyncObjects();

    /**
     * @brief 创建描述符布局
     *
     * @return 是否创建成功
     */
    bool createDescriptorLayouts();
    /**
     * @brief 创建描述符池和描述符集
     *
     * @return 是否创建成功
     */
    bool createDescriptorPoolAndSets();

    /**
     * @brief 创建视频渲染管线
     *
     * @return 是否创建成功
     */
    bool createVideoPipeline();
    /**
     * @brief 创建OSD图层渲染管线
     *
     * @return 是否创建成功
     */
    bool createOsdPipeline();

    /**
     * @brief 创建视频区域（顶点数组）
     *
     * @return 是否创建成功
     */
    bool createVideoGeometry();
    /**
     * @brief 创建OSD图层区域（顶点数组）
     *
     * @return 是否创建成功
     */
    bool createOsdGeometry();

    /**
     * @brief 销毁当前交换链相关对象
     */
    void destroySwapchainObjects();
    /**
     * @brief 重建交换链
     *
     * @return 是否重建成功
     */
    bool recreateSwapchain();

    /**
     * @brief 加载着色器
     *
     * @param path 文件路径
     * @return 着色器代码
     */
    std::vector<uint32_t> readSpvFile(const std::string &path) const;
    /**
     * @brief 创建着色器模块
     *
     * @param code 着色器代码
     * @return 着色器模块
     */
    VkShaderModule createShaderModule(const std::vector<uint32_t> &code) const;

    /**
     * @brief 确保着色器二进制文件已生成
     *
     * @param glslRelativePath 着色器文本文件相对路径
     * @param spvRelativePath 着色器二进制文件相对路径
     * @param shaderKind 着色器类型
     * @return 是否生成
     */
    bool ensureShaderSpv(const std::string &glslRelativePath, const std::string &spvRelativePath,
                         int shaderKind) const;

    /**
     * @brief 根据传入的相对路径，得到着色器绝对路径
     *
     * @param relative 相对路径
     * @return 对应的绝对路径
     */
    std::filesystem::path shaderPath(const std::string &relative) const;

    /**
     * @brief 确保OSD纹理已生成
     *
     * @param osdTextUtf8 OSD时间戳字符串
     */
    void ensureOsdTexture(const std::string &osdTextUtf8);

    /**
     * @brief 确保一个足够大的、CPU可写的buffer已生成，用来上传数据，如OSD纹理
     *
     * @param requiredSize 需要的大小
     * @param buffer buffer句柄
     * @param alloc 内存分配器
     * @param mapped CPU映射地址
     * @param usage buffer的用途
     * @return
     */
    void ensureStagingBuffer(VkDeviceSize requiredSize, VkBuffer &buffer, VmaAllocation &alloc,
                             void *&mapped, VkBufferUsageFlags usage);

    /**
     * @brief 图像布局转换，确保同步。src完成后，才会执行dst的操作
     *
     * @param cmd 记录命令的队列
     * @param image 图像
     * @param oldLayout 旧的内存布局
     * @param newLayout 新的内存布局
     * @param srcStage 源pipeline stage(管线阶段)
     * @param dstStage 新pipeline stage(管线阶段)
     * @param srcAccess 源操作
     * @param dstAccess 新操作
     * @param aspectMask 图像哪一部分被访问/操作
     */
    void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                         VkImageLayout newLayout, VkPipelineStageFlags srcStage,
                         VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                         VkAccessFlags dstAccess, VkImageAspectFlags aspectMask) const;

    /**
     * @brief 拷贝数据缓冲到图像
     *
     * @param cmd 命令队列
     * @param buffer 数据缓冲
     * @param image 图像
     * @param width 图像宽
     * @param height 图像高
     * @param aspectMask 图像哪一部分被访问/操作
     */
    void copyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer, VkImage image, uint32_t width,
                           uint32_t height, VkImageAspectFlags aspectMask) const;

private:
    // CUDA => vulkan 互操作所需资源
    struct CudaInteropSlot {
        // 宽度
        uint32_t width = 0;
        // 高度
        uint32_t height = 0;
        // Y平面的步幅，用来对齐
        int strideY = 0;
        // UV平面的步幅，用来对齐
        int strideUV = 0;

        // 存放Y平面的数据
        VkImage yImage = VK_NULL_HANDLE;
        // Y平面的当前布局
        VkImageLayout yLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // 存放Y平面数据的实际内存区域
        VkDeviceMemory yMemory = VK_NULL_HANDLE;
        // 指示vulkan该如何访问Y平面数据
        VkImageView yView = VK_NULL_HANDLE;

        // 存放UV平面的数据
        VkImage uvImage = VK_NULL_HANDLE;
        // UV平面的当前布局
        VkImageLayout uvLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // 存放UV平面数据的实际内存区域
        VkDeviceMemory uvMemory = VK_NULL_HANDLE;
        // 指示vulkan该如何访问UV平面数据
        VkImageView uvView = VK_NULL_HANDLE;

        // CUDA数据写入成功信号，和cudaExtSemaphore是同一个同步原语
        VkSemaphore cudaReadySemaphore = VK_NULL_HANDLE;

        // Y平面导出的内存
        CUexternalMemory yExtMem = nullptr;
        // UV平面导出的内存
        CUexternalMemory uvExtMem = nullptr;
        // Y平面数据的显存指针
        CUmipmappedArray yPtr = 0;
        // UV平面数据的显存指针
        CUmipmappedArray uvPtr = 0;
        // cuda导出的信号量，用于vulkan和cuda之间的同步，表明数据写入成功
        CUexternalSemaphore cudaExtSemaphore = nullptr;
    };

    // vulkan => CUDA 互操作所需资源
    struct VulkanInteropSlot {
        // 宽度
        uint32_t width = 0;
        // 高度
        uint32_t height = 0;
        // 平面的步幅，用来对齐
        int stride = 0;

        // 存放平面的数据
        VkImage image = VK_NULL_HANDLE;
        // 平面的当前布局
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        // 存放平面数据的实际内存区域
        VkDeviceMemory memory = VK_NULL_HANDLE;
        // 指示vulkan该如何访问平面数据
        VkImageView view = VK_NULL_HANDLE;
        // 帧缓冲
        VkFramebuffer framebuffer = VK_NULL_HANDLE;

        // CUDA数据写入成功信号，和cudaExtSemaphore是同一个同步原语
        VkSemaphore cudaReadySemaphore = VK_NULL_HANDLE;

        // 平面导出的内存
        CUexternalMemory extMem = nullptr;
        // Y平面数据的显存指针
        CUmipmappedArray cuArrayPtr = 0;
        // cuda导出的信号量，用于vulkan和cuda之间的同步，表明数据写入成功
        CUexternalSemaphore cudaExtSemaphore = nullptr;
    };

    /**
     * @brief 初始化CUDA互操作资源
     *
     * @return 是否初始化成功
     */
    bool initCudaInterop();
    /**
     * @brief 安全释放互操作资源
     *
     */
    void shutdownCudaInterop();
    /**
     * @brief 确保CUDA互操作资源已准备好
     *
     * @param slot 互操作资源
     * @param width 帧的宽度
     * @param height 帧的高度
     * @param strideY Y平面步幅
     * @param strideUV UV平面步幅
     * @return 是否准备好
     */
    bool ensureCudaInteropSlot(CudaInteropSlot &slot, uint32_t width, uint32_t height, int strideY,
                               int strideUV);
    /**
     * @brief 创建可导出的数据缓冲区，供其它API访问，如CUDA往里填充数据
     *
     * @param size 缓冲区大小
     * @param usage 使用方式
     * @param image 图片
     * @param memory 显存
     * @return
     */
    bool createExportableImage(uint32_t width, uint32_t height, VkFormat format, VkImage &image,
                               VkDeviceMemory &memory);
    /**
     * @brief 导出显存句柄，供外部使用
     *
     * @param memory 显存
     * @param handle 句柄
     * @return 是否导出成功
     */
    bool exportMemoryHandle(VkDeviceMemory memory, HANDLE &handle) const;
    /**
     * @brief 导出信号量句柄，供外部使用
     *
     * @param semaphore 信号量
     * @param handle 句柄
     * @return 是否导出成功
     */
    bool exportSemaphoreHandle(VkSemaphore semaphore, HANDLE &handle) const;
    /**
     * @brief 在 GPU 支持的内存类型中，找到一个同时满足 “资源要求” 和 “指定属性” 的 memory type
     * index。
     *
     * @param typeBits 资源允许的 memory type
     * @param properties 所需的属性
     * @return
     */
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
    /**
     * @brief 创建离屏RGBA纹理及其相关资源
     *
     * @return 是否创建成功
     */
    bool createOffscreenResources();
    /**
     * @brief 销毁离屏RGBA纹理及其相关资源
     */
    void destroyOffscreenResources();
    /**
     * @brief 创建将离屏纹理blitting到交换链的管线
     *
     * @return 是否创建成功
     */
    bool createBlitPipeline();

private:
    // BlitVertex结构体：用于全屏blit和YUV转换的顶点
    struct BlitVertex {
        float pos[2];
        float uv[2];
    };

    // 是否已初始化
    bool initialized_ = false;
    // 窗口指针
    GLFWwindow *window_ = nullptr;
    // 窗口大小是否发生改变
    bool windowResized_ = false;

    // 应用程序所在目录
    std::filesystem::path exeDir_;

    // vulkan实例
    vkb::Instance instance_{};
    // vulkan渲染表面
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    // 物理设备
    vkb::PhysicalDevice physicalDevice_{};
    // 逻辑设备
    vkb::Device device_{};
    // 图形队列
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    // 呈现队列
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    // 图形队列族索引
    uint32_t graphicsQueueFamilyIndex_ = 0;
    // 呈现队列族索引
    uint32_t presentQueueFamilyIndex_ = 0;

    // 交换链
    vkb::Swapchain swapchain_{};
    // 渲染通道
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    // 纹理缓冲
    std::vector<VkFramebuffer> framebuffers_;
    // 图像视图
    std::vector<VkImageView> swapchainImageViews_;

    // 命令池
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    // 命令缓冲区
    std::vector<VkCommandBuffer> commandBuffers_;

    // 最大飞行帧数量
    static constexpr uint32_t kMaxFramesInFlight = 2;
    // 当前帧的索引
    uint32_t currentFrame_ = 0;
    // 上一帧的索引
    uint32_t lastFrame_ = 0;
    // 图像是否可用的信号，即准备好可读
    std::vector<VkSemaphore> imageAvailableSemaphores_;
    // 图像是否已渲染完成的信号，即准备好可写
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    // 标识帧正在被GPU使用的fence，大小和kMaxFramesInFlight一致
    std::vector<VkFence> inFlightFences_;
    // 记录交换链中的图像，对应的是哪一个fence，大小和swapchainImageViews_一致
    std::vector<VkFence> imagesInFlight_;

    // 内存分配器
    VmaAllocator allocator_ = VK_NULL_HANDLE;

    // 渲染视频的描述符布局
    VkDescriptorSetLayout videoSetLayout_ = VK_NULL_HANDLE;
    // 渲染OSD的描述符布局
    VkDescriptorSetLayout osdSetLayout_ = VK_NULL_HANDLE;
    // 描述符池
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    // 渲染视频的描述符集
    std::vector<VkDescriptorSet> videoDescriptorSets_;
    // 渲染OSD的描述符集
    std::vector<VkDescriptorSet> osdDescriptorSets_;
    // 视频描述符是否可用
    std::vector<bool> videoDescriptorsValid_;
    // 上一帧视频对应的描述符集索引
    uint32_t lastVideoSetIndex_ = 0;
    // 是否有视频描述符集
    bool hasVideoSet_ = false;

    // 视频渲染管线布局
    VkPipelineLayout videoPipelineLayout_ = VK_NULL_HANDLE;
    // 视频渲染管线
    VkPipeline videoPipeline_ = VK_NULL_HANDLE;

    // OSD渲染管线布局
    VkPipelineLayout osdPipelineLayout_ = VK_NULL_HANDLE;
    // OSD渲染管线
    VkPipeline osdPipeline_ = VK_NULL_HANDLE;

    // 渲染视频的顶点缓冲器
    VkBuffer videoVertexBuffer_ = VK_NULL_HANDLE;
    // 分配器
    VmaAllocation videoVertexAlloc_ = VK_NULL_HANDLE;

    // 渲染OSD的顶点缓冲区
    VkBuffer osdVertexBuffer_ = VK_NULL_HANDLE;
    // 分配器
    VmaAllocation osdVertexAlloc_ = VK_NULL_HANDLE;
    // OSD上传时可用的CPU地址
    void *osdVertexMapped_ = nullptr;

    // 互操作资源
    std::vector<CudaInteropSlot> cudaSlots_;
    std::vector<VulkanInteropSlot> vulkanSlots_;
    // cuda上下文
    CUcontext cudaContext_ = nullptr;
    // cuda工作流
    CUstream cudaStream_ = nullptr;
    // 获取的待渲染的视频帧
    decoder_sdk::Frame pendingCudaFrame_;
    // 是否有视频帧需要渲染
    bool cudaFramePending_ = false;

    // 由于编码需求，离屏大小与编码器一致
    uint32_t offscreenWidth_ = 0;
    uint32_t offscreenHeight_ = 0;
    // ========== 离屏渲染相关资源 ==========
    // 离屏纹理的渲染通道
    VkRenderPass offscreenRenderPass_ = VK_NULL_HANDLE;
    // 离屏纹理的采样器
    VkSampler offscreenSampler_ = VK_NULL_HANDLE;
    // 用于将离屏纹理渲染到交换链的描述符布局
    VkDescriptorSetLayout blitSetLayout_ = VK_NULL_HANDLE;
    // 用于将离屏纹理渲染到交换链的描述符集
    std::vector<VkDescriptorSet> blitDescriptorSets_;
    // Blit管线布局
    VkPipelineLayout blitPipelineLayout_ = VK_NULL_HANDLE;
    // Blit管线
    VkPipeline blitPipeline_ = VK_NULL_HANDLE;
    // Blit顶点缓冲区（全屏四边形）
    VkBuffer blitVertexBuffer_ = VK_NULL_HANDLE;
    // Blit顶点缓冲分配器
    VmaAllocation blitVertexAlloc_ = VK_NULL_HANDLE;

    // OSD文字渲染器
    OsdTextRendererStb osdTextRenderer_;
    // 上一个OSD文字
    std::string lastOsdText_;
    // OSD图像
    VkImage osdImage_ = VK_NULL_HANDLE;
    // OSD图像分配器
    VmaAllocation osdImageAlloc_ = VK_NULL_HANDLE;
    // OSD图像视图
    VkImageView osdImageView_ = VK_NULL_HANDLE;
    // OSD图像采样器
    VkSampler osdSampler_ = VK_NULL_HANDLE;
    // 视频图像采样器
    VkSampler videoSampler_ = VK_NULL_HANDLE;
    // OSD图像的布局
    VkImageLayout osdImageLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    // OSD图像宽度
    uint32_t osdWidth_ = 0;
    // OSD图像高度
    uint32_t osdHeight_ = 0;

    // 足够大的OSD上传所用的缓冲区
    VkBuffer osdStagingBuffer_ = VK_NULL_HANDLE;
    // 缓冲区分配器
    VmaAllocation osdStagingAlloc_ = VK_NULL_HANDLE;
    // 映射到CPU的地址
    void *osdStagingMapped_ = nullptr;
    // 缓冲区大小
    VkDeviceSize osdStagingSize_ = 0;
    // 是否需要上传OSD缓冲区
    bool osdUploadPending_ = false;
};
