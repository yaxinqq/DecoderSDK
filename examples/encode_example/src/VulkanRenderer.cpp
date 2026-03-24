#include "VulkanRenderer.h"

#include <shaderc/shaderc.hpp>

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
// 获得可执行程序所在的文件夹路径
static std::filesystem::path getExecutableDir()
{
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len == 0) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::string(buf, buf + len)).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

// 读取文本文件
static bool readTextFile(const std::filesystem::path &path, std::string &out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        return false;
    }

    // 获取总大小
    const std::streamsize size = f.tellg();
    if (size <= 0) {
        return false;
    }

    // 分配内存并写入
    out.resize(static_cast<size_t>(size));
    f.seekg(0);
    f.read(out.data(), size);
    return true;
}

// 写入二进制文件
static bool writeBinaryFile(const std::filesystem::path &path, const std::vector<uint32_t> &words)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        return false;
    }
    f.write(reinterpret_cast<const char *>(words.data()),
            static_cast<std::streamsize>(words.size() * sizeof(uint32_t)));
    return true;
}

// 向信号量数组尾部追加一个有效项，同时维护数量计数
static bool appendSemaphore(VkSemaphore semaphore, VkPipelineStageFlags stage, VkSemaphore *sems,
                            VkPipelineStageFlags *stages, uint32_t capacity, uint32_t &count)
{
    if (semaphore == VK_NULL_HANDLE || count >= capacity) {
        return false;
    }

    sems[count] = semaphore;
    stages[count] = stage;
    ++count;
    return true;
}
} // namespace

VulkanRenderer::VulkanRenderer() = default;

VulkanRenderer::~VulkanRenderer()
{
    // 析构时保证安全退出
    shutdown();
}

bool VulkanRenderer::initialize(uint32_t width, uint32_t height, VkFormat imageFormat, bool debug)
{
    if (initialized_) {
        return true;
    }

    // 加载osd渲染器的字体
    if (!osdTextRenderer_.tryLoadDefaultChineseFont()) {
        std::cerr << "[OSD] Failed to load a default Chinese font from Windows fonts directory."
                  << std::endl;
    }

    // 设置离屏渲染宽
    offscreenWidth_ = width;
    // 设置离屏渲染高
    offscreenHeight_ = height;
    // 设置离屏渲染图像格式
    offscreenFormat_ = imageFormat;
    // 设置是否为调试模式
    debug_ = debug;
    // 得到exe所在的目录
    exeDir_ = getExecutableDir();

    // 初始化窗口
    if (!initWindow(width, height, "Encode Demo Debug Mode")) {
        return false;
    }
    // 初始化vulkan资源，未成功则释放已成功申请的部分
    if (!initVulkan()) {
        shutdown();
        return false;
    }

    // 初始化OSD纹理
    ensureOsdTexture(std::string{});

    // 初始化完成
    initialized_ = true;
    startTime_ = std::chrono::steady_clock::now();
    return true;
}

void VulkanRenderer::setMaxRunning(uint32_t runningTime)
{
    maxRunningTime_ = runningTime;
}

void VulkanRenderer::shutdown()
{
    if (!window_ && !device_.device) {
        return;
    }

    // 如果当前存在已用设备，则等待设备任务完成
    if (device_.device) {
        vkDeviceWaitIdle(device_.device);
    }

    // 销毁CUDA的互操作资源
    shutdownCudaInterop();

    // 销毁离屏渲染资源
    destroyOffscreenResources();

    // 销毁顶点缓冲区
    if (osdVertexBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, osdVertexBuffer_, osdVertexAlloc_);
        osdVertexBuffer_ = VK_NULL_HANDLE;
        osdVertexAlloc_ = VK_NULL_HANDLE;
        osdVertexMapped_ = nullptr;
    }
    if (videoVertexBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, videoVertexBuffer_, videoVertexAlloc_);
        videoVertexBuffer_ = VK_NULL_HANDLE;
        videoVertexAlloc_ = VK_NULL_HANDLE;
    }

    // 销毁OSD的上传缓冲区以及采样器
    if (osdStagingBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, osdStagingBuffer_, osdStagingAlloc_);
        osdStagingBuffer_ = VK_NULL_HANDLE;
        osdStagingAlloc_ = VK_NULL_HANDLE;
        osdStagingMapped_ = nullptr;
        osdStagingSize_ = 0;
    }
    if (osdSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_.device, osdSampler_, nullptr);
        osdSampler_ = VK_NULL_HANDLE;
    }
    if (videoSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_.device, videoSampler_, nullptr);
        videoSampler_ = VK_NULL_HANDLE;
    }

    // 销毁OSD图像相关
    if (osdImageView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_.device, osdImageView_, nullptr);
        osdImageView_ = VK_NULL_HANDLE;
    }
    if (osdImage_ != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, osdImage_, osdImageAlloc_);
        osdImage_ = VK_NULL_HANDLE;
        osdImageAlloc_ = VK_NULL_HANDLE;
    }

    // 销毁管线
    if (videoPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_.device, videoPipeline_, nullptr);
        videoPipeline_ = VK_NULL_HANDLE;
    }
    if (videoPipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_.device, videoPipelineLayout_, nullptr);
        videoPipelineLayout_ = VK_NULL_HANDLE;
    }

    if (osdPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_.device, osdPipeline_, nullptr);
        osdPipeline_ = VK_NULL_HANDLE;
    }
    if (osdPipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_.device, osdPipelineLayout_, nullptr);
        osdPipelineLayout_ = VK_NULL_HANDLE;
    }

    // 销毁描述符资源
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_.device, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    if (videoSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.device, videoSetLayout_, nullptr);
        videoSetLayout_ = VK_NULL_HANDLE;
    }
    if (osdSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.device, osdSetLayout_, nullptr);
        osdSetLayout_ = VK_NULL_HANDLE;
    }

    // 销毁命令缓冲区、池
    if (!commandBuffers_.empty() && commandPool_ != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_.device, commandPool_,
                             static_cast<uint32_t>(commandBuffers_.size()), commandBuffers_.data());
        commandBuffers_.clear();
    }
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_.device, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }

    // 销毁同步对象
    for (auto fence : inFlightFences_) {
        if (fence != VK_NULL_HANDLE) {
            vkDestroyFence(device_.device, fence, nullptr);
        }
    }
    for (auto sem : imageAvailableSemaphores_) {
        if (sem != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_.device, sem, nullptr);
        }
    }
    for (auto sem : renderFinishedSemaphores_) {
        if (sem != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_.device, sem, nullptr);
        }
    }
    inFlightFences_.clear();
    imageAvailableSemaphores_.clear();
    renderFinishedSemaphores_.clear();
    imagesInFlight_.clear();

    // 销毁交换链相关对象
    destroySwapchainObjects();

    // 销毁渲染管道
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_.device, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }

    // 销毁内存分配器
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }

    // 销毁设备、渲染表面、实例
    if (device_.device) {
        vkb::destroy_device(device_);
        device_ = {};
    }
    if (surface_ != VK_NULL_HANDLE && instance_.instance) {
        vkDestroySurfaceKHR(instance_.instance, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (instance_.instance) {
        vkb::destroy_instance(instance_);
        instance_ = {};
    }

    // 销毁窗口
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
    }

    initialized_ = false;
}

bool VulkanRenderer::shouldClose() const
{
    if (!debug_) {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                startTime_)
                   .count() >= maxRunningTime_;
    }

    // 判断是否应该关闭
    return !window_ || glfwWindowShouldClose(window_);
}

void VulkanRenderer::pollEvents()
{
    // 处理鼠标、键盘等系统事件
    if (window_) {
        glfwPollEvents();
    }
}

bool VulkanRenderer::initWindow(uint32_t width, uint32_t height, const char *title)
{
    if (!debug_)
        return true;

    // 初始化glfw
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window_ = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height), title, nullptr,
                               nullptr);
    if (!window_) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return false;
    }

    // 设置窗口大小改变的回调函数和用户数据
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, [](GLFWwindow *w, int, int) {
        auto *self = static_cast<VulkanRenderer *>(glfwGetWindowUserPointer(w));
        if (self) {
            // 当窗口大小改变时，需要重建交换链
            self->windowResized_ = true;
        }
    });
    return true;
}

bool VulkanRenderer::initVulkan()
{
    if (!createInstance())
        return false;
    if (!createSurface())
        return false;
    if (!pickPhysicalDevice())
        return false;
    if (!createDevice())
        return false;
    if (!createAllocator())
        return false;
    if (!createSwapchain())
        return false;
    if (!createRenderPass())
        return false;
    if (!createFramebuffers())
        return false;
    if (!createCommandPool())
        return false;
    if (!createDescriptorLayouts())
        return false;
    if (!createDescriptorPoolAndSets())
        return false;
    if (!createOffscreenResources())
        return false;
    if (!createVideoPipeline())
        return false;
    if (!createOsdPipeline())
        return false;
    if (!createVideoGeometry())
        return false;
    if (!createOsdGeometry())
        return false;
    if (!createSyncObjects())
        return false;
    return true;
}

bool VulkanRenderer::createInstance()
{
    vkb::InstanceBuilder builder;
    builder.set_app_name("Encode Demo");
    builder.require_api_version(1, 1, 0);

#if !defined(NDEBUG)
    builder.request_validation_layers(true);
    builder.use_default_debug_messenger();
#else
    builder.request_validation_layers(false);
#endif

    // 得到glfw窗口所需要的vulkan扩展，否则设为无头模式
    if (debug_) {
        uint32_t glfwExtCount = 0;
        const char **glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        for (uint32_t i = 0; i < glfwExtCount; ++i) {
            builder.enable_extension(glfwExts[i]);
        }
    } else {
        builder.set_headless();
    }

    // 构造vulkan实例
    auto instRet = builder.build();
    if (!instRet) {
        std::cerr << "Failed to create Vulkan instance: " << instRet.error().message() << std::endl;
        return false;
    }
    instance_ = instRet.value();
    return true;
}

bool VulkanRenderer::createSurface()
{
    if (!debug_)
        return true;

    // 创建渲染表面
    if (!window_) {
        return false;
    }
    if (glfwCreateWindowSurface(instance_.instance, window_, nullptr, &surface_) != VK_SUCCESS) {
        std::cerr << "Failed to create window surface" << std::endl;
        return false;
    }
    return true;
}

bool VulkanRenderer::pickPhysicalDevice()
{
    // 选择物理设备
    vkb::PhysicalDeviceSelector selector{instance_};
    if (debug_) {
        selector.set_surface(surface_).add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    // 加入所需的扩展
#if defined(_WIN32)
    selector.add_required_extension(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME)
        .add_required_extension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)
        .add_required_extension(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME)
        .add_required_extension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
#endif

    // 得到合适的物理设备
    auto physRet = selector.select();
    if (!physRet) {
        std::cerr << "Failed to select physical device: " << physRet.error().message() << std::endl;
        return false;
    }
    physicalDevice_ = physRet.value();
    return true;
}

bool VulkanRenderer::createDevice()
{
    // 创建合适的逻辑设备
    vkb::DeviceBuilder deviceBuilder{physicalDevice_};
    auto devRet = deviceBuilder.build();
    if (!devRet) {
        std::cerr << "Failed to create device: " << devRet.error().message() << std::endl;
        return false;
    }
    device_ = devRet.value();

    // 得到图像队列、呈现队列，以及相关的队列族索引
    const auto gq = device_.get_queue(vkb::QueueType::graphics);
    const auto pq = device_.get_queue(vkb::QueueType::present);
    const auto gqi = device_.get_queue_index(vkb::QueueType::graphics);
    const auto pqi = device_.get_queue_index(vkb::QueueType::present);
    if (!gq.has_value() || (debug_ && !pq.has_value()) || !gqi.has_value() ||
        (debug_ && !pqi.has_value())) {
        std::cerr << "Failed to get device queues" << std::endl;
        return false;
    }

    graphicsQueue_ = gq.value();
    graphicsQueueFamilyIndex_ = gqi.value();

    if (debug_) {
        presentQueue_ = pq.value();
        presentQueueFamilyIndex_ = pqi.value();
    }
    return true;
}

bool VulkanRenderer::createAllocator()
{
    // 创建VMA内存分配器
    VmaVulkanFunctions funcs{};
    funcs.vkGetInstanceProcAddr = instance_.fp_vkGetInstanceProcAddr;
    funcs.vkGetDeviceProcAddr = instance_.fp_vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo ci{};
    ci.instance = instance_.instance;
    ci.physicalDevice = physicalDevice_.physical_device;
    ci.device = device_.device;
    ci.pVulkanFunctions = &funcs;

    if (vmaCreateAllocator(&ci, &allocator_) != VK_SUCCESS) {
        std::cerr << "Failed to create VMA allocator" << std::endl;
        return false;
    }
    return true;
}

bool VulkanRenderer::createSwapchain()
{
    if (!debug_)
        return true;

    // 创建合适的交换链
    vkb::SwapchainBuilder builder{physicalDevice_, device_, surface_};
    auto scRet = builder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                     .set_desired_format({offscreenFormat_, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                     .set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                     .build();
    if (!scRet) {
        std::cerr << "Failed to create swapchain: " << scRet.error().message() << std::endl;
        return false;
    }
    swapchain_ = scRet.value();

    // 得到交换链中所有vkImageViews的实例
    auto views = swapchain_.get_image_views();
    if (!views.has_value()) {
        std::cerr << "Failed to get swapchain image views" << std::endl;
        return false;
    }
    swapchainImageViews_ = views.value();
    return true;
}

bool VulkanRenderer::createRenderPass()
{
    if (!debug_)
        return true;

    // 声明图像附件（渲染目标），图像格式和交换链的图像格式一致
    // loadOp - RenderPass开始时，对颜色+深度的处理。清空所有数据
    // storeOp - RenderPass结束后，对颜色+深度的处理。将渲染结果保存在图像中
    // stencilLoadOp - RenderPass开始时，对模板数据的处理。不关心
    // stencilStoreOp - RenderPass结束后，对模板数据的处理。不关心
    // initialLayout - RenderPass开始时，这张图像在GPU中的使用状态或访问模式。未定义
    // initialLayout - RenderPass结束时，这张图像在GPU中的使用状态或访问模式。用来呈现（渲染到屏幕）
    VkAttachmentDescription color{};
    color.format = swapchain_.image_format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // 图像附件的映射，供subpass使用
    // attachment - subpass使用的renderpass中的图像附件索引
    // layout - subpass使用时的访问方式
    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // renderpass的处理过程
    // 指定使用哪个图像附件
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    // 子通道依赖，从外部 => 第一个子通道
    // srcSubpass - 源（上一个）子通道；特殊值，和外部操作同步
    // dstSubpass - 目标（当前）子通道；第一个子通道（索引值）
    // srcStageMask - 源子通道管线阶段，完成后才能到dstStageMask
    // dstStageMask - 目标子通道管线阶段
    // dstAccessMask - 在这个阶段中，对图像附件的使用方式
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    // 创建渲染通道
    // attachmentCount - 绑定的图像附件数量
    // pAttachments - 图像附件指针
    // subpassCount - 子通道（处理过程）数量
    // pSubpasses - 子通道指针
    // dependencyCount - 子通道依赖数量
    // pDependencies - 子通道依赖指针
    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;

    if (vkCreateRenderPass(device_.device, &rp, nullptr, &renderPass_) != VK_SUCCESS) {
        std::cerr << "Failed to create render pass" << std::endl;
        return false;
    }
    return true;
}

bool VulkanRenderer::createOffscreenResources()
{
    // ========== 创建离屏纹理的渲染通道 ==========
    // 离屏纹理格式与交换链一致，使用BGRA格式以避免render pass不兼容错误
    VkAttachmentDescription colorAttach{};
    colorAttach.format = offscreenFormat_;
    colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttach.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // 图像附件的映射
    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // 子通道定义
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    // 子通道依赖
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    // 创建渲染通道
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &colorAttach;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;

    if (vkCreateRenderPass(device_.device, &rpci, nullptr, &offscreenRenderPass_) != VK_SUCCESS) {
        std::cerr << "Failed to create offscreen render pass" << std::endl;
        return false;
    }

    // 创建离屏纹理的采样器
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;

    if (vkCreateSampler(device_.device, &samplerInfo, nullptr, &offscreenSampler_) != VK_SUCCESS) {
        std::cerr << "Failed to create offscreen sampler" << std::endl;
        return false;
    }

    return true;
}

bool VulkanRenderer::ensureOffscreenResources()
{
    if (!vulkanSlots_.empty() && vulkanSlots_[0].width == offscreenWidth_ &&
        vulkanSlots_[0].height == offscreenHeight_) {
        return true;
    }

    if (cuCtxPushCurrent(cudaContext_) != CUDA_SUCCESS) {
        return false;
    }

    // 销毁各类互操作资源
    for (auto &slot : vulkanSlots_) {
        if (slot.framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_.device, slot.framebuffer, nullptr);
        }

        if (slot.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_.device, slot.view, nullptr);
            slot.view = VK_NULL_HANDLE;
        }
        if (slot.image != VK_NULL_HANDLE) {
            vkDestroyImage(device_.device, slot.image, nullptr);
            slot.image = VK_NULL_HANDLE;
            slot.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        if (slot.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_.device, slot.memory, nullptr);
            slot.memory = VK_NULL_HANDLE;
        }
        if (slot.cudaReadySemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_.device, slot.cudaReadySemaphore, nullptr);
            slot.cudaReadySemaphore = VK_NULL_HANDLE;
        }

        if (slot.extMem) {
            cuDestroyExternalMemory(slot.extMem);
            slot.extMem = nullptr;
            slot.cuArrayPtr = 0;
        }
        if (slot.cudaExtSemaphore) {
            cuDestroyExternalSemaphore(slot.cudaExtSemaphore);
            slot.cudaExtSemaphore = nullptr;
        }
        slot.width = 0;
        slot.height = 0;
    }
    vulkanSlots_.clear();

    // 离屏纹理尺寸优先使用编码器配置，若未配置则使用交换链尺寸
    uint32_t width = (offscreenWidth_ > 0) ? offscreenWidth_ : 1280;
    uint32_t height = (offscreenHeight_ > 0) ? offscreenHeight_ : 720;
    if (width == 0 || height == 0) {
        return false;
    }

    // 为每个飞行帧创建离屏资源
    vulkanSlots_.resize(kMaxFramesInFlight);

    // 声明图像支持 Windows handle 导出（用于 CUDA interop）
    VkExternalMemoryImageCreateInfo externalImageInfo{};
    externalImageInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalImageInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    // 创建离屏纹理
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalImageInfo; // 链接导出信息
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = offscreenFormat_;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    for (size_t i = 0; i < kMaxFramesInFlight; ++i) {
        auto &slot = vulkanSlots_[i];

        // 创建离屏纹理图像（直接使用 vkCreateImage 以支持导出）
        if (vkCreateImage(device_.device, &imageInfo, nullptr, &slot.image) != VK_SUCCESS) {
            std::cerr << "Failed to create offscreen image " << i << std::endl;
            return false;
        }

        // 获取图像的内存需求
        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(device_.device, slot.image, &memRequirements);

        // 声明内存支持 Windows handle 导出
        VkExportMemoryAllocateInfo exportMemInfo{};
        exportMemInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exportMemInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

        // 分配支持导出的图像内存
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.pNext = &exportMemInfo; // 链接导出信息
        allocateInfo.allocationSize = memRequirements.size;
        allocateInfo.memoryTypeIndex =
            findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device_.device, &allocateInfo, nullptr, &slot.memory) != VK_SUCCESS) {
            std::cerr << "Failed to allocate offscreen image memory " << i << std::endl;
            vkDestroyImage(device_.device, slot.image, nullptr);
            return false;
        }

        // 绑定内存到图像
        if (vkBindImageMemory(device_.device, slot.image, slot.memory, 0) != VK_SUCCESS) {
            std::cerr << "Failed to bind offscreen image memory " << i << std::endl;
            vkFreeMemory(device_.device, slot.memory, nullptr);
            vkDestroyImage(device_.device, slot.image, nullptr);
            return false;
        }

        // 创建离屏纹理的视图
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = slot.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = offscreenFormat_;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_.device, &viewInfo, nullptr, &slot.view) != VK_SUCCESS) {
            std::cerr << "Failed to create offscreen image view " << i << std::endl;
            return false;
        }

        // 创建离屏纹理的帧缓冲
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = offscreenRenderPass_;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &slot.view;
        fbInfo.width = width;
        fbInfo.height = height;
        fbInfo.layers = 1;

        if (vkCreateFramebuffer(device_.device, &fbInfo, nullptr, &slot.framebuffer) !=
            VK_SUCCESS) {
            std::cerr << "Failed to create offscreen framebuffer " << i << std::endl;
            return false;
        }

        // 共享信号量不存在，则创建
        if (slot.cudaReadySemaphore == VK_NULL_HANDLE) {
            // 创建CUDA数据写入成功的信号量，由vulkan导出给CUDA使用
            VkExportSemaphoreCreateInfo exportSem{};
            exportSem.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
            exportSem.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

            VkSemaphoreCreateInfo sci{};
            sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            sci.pNext = &exportSem;
            if (vkCreateSemaphore(device_.device, &sci, nullptr, &slot.cudaReadySemaphore) !=
                VK_SUCCESS) {
                return false;
            }
        }

        // 导出信号量不存在则创建
        if (!slot.cudaExtSemaphore) {
            // 导出信号量
            HANDLE semHandle = nullptr;
            if (!exportSemaphoreHandle(slot.cudaReadySemaphore, semHandle) || !semHandle) {
                return false;
            }

            // 当前上下文设置不成功时，需要关闭句柄
            if (cuCtxPushCurrent(cudaContext_) != CUDA_SUCCESS) {
                CloseHandle(semHandle);
                return false;
            }

            // CUDA外部导出信号量的声明
            // type - 类型
            // handle - 句柄
            CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC desc{};
            desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32;
            desc.handle.win32.handle = semHandle;

            // 导入外部信号量
            // 弹出上下文，并关闭句柄
            const CUresult r = cuImportExternalSemaphore(&slot.cudaExtSemaphore, &desc);
            CloseHandle(semHandle);

            if (r != CUDA_SUCCESS) {
                slot.cudaExtSemaphore = nullptr;
                return false;
            }
        }

        // 得到导出数据缓冲区的句柄
        HANDLE handle = nullptr;
        if (!exportMemoryHandle(slot.memory, handle) || !handle) {
            if (handle)
                CloseHandle(handle);

            CUcontext popped = nullptr;
            cuCtxPopCurrent(&popped);
            return false;
        }

        // CUDA导入句柄，得到映射后的数据区域
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC desc{};
        desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
        desc.handle.win32.handle = handle;
        desc.size = memRequirements.size;

        CUresult ret = cuImportExternalMemory(&slot.extMem, &desc);

        // 关闭句柄
        if (handle) {
            CloseHandle(handle);
        }

        // 如果没有导入成功，则清理已声明成功的部分
        if (ret != CUDA_SUCCESS) {
            if (slot.extMem) {
                cuDestroyExternalMemory(slot.extMem);
                slot.extMem = nullptr;
            }

            CUcontext popped = nullptr;
            cuCtxPopCurrent(&popped);
            return false;
        }

        // 对外部区域进行映射，得到可操作地址
        CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC arrayDesc{};
        arrayDesc.offset = 0;
        arrayDesc.arrayDesc.Width = width;
        arrayDesc.arrayDesc.Height = height;
        arrayDesc.arrayDesc.Depth = 0;
        arrayDesc.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
        arrayDesc.arrayDesc.NumChannels = 4;
        arrayDesc.numLevels = 1;

        if (cuExternalMemoryGetMappedMipmappedArray(&slot.cuArrayPtr, slot.extMem, &arrayDesc) !=
            CUDA_SUCCESS) {
            CUcontext popped = nullptr;
            cuCtxPopCurrent(&popped);
            return false;
        }

        slot.width = width;
        slot.height = height;
    }

    CUcontext popped = nullptr;
    cuCtxPopCurrent(&popped);

    // 创建离屏渲染所需的资源
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        if (!createBlitPipeline())
            return false;
    }

    return true;
}

bool VulkanRenderer::fillEncodedFrame(decoder_sdk::Frame &frame)
{
    // 检查前置条件
    if (!cudaContext_ || !device_.device || vulkanSlots_.empty()) {
        return false;
    }

    // 获取当前帧索引
    const uint32_t frameIndex = lastFrame_;
    if (frameIndex >= vulkanSlots_.size()) {
        return false;
    }

    vkWaitForFences(device_.device, 1, &inFlightFences_[frameIndex], VK_TRUE, UINT64_MAX);

    // 拷贝
    auto &slot = vulkanSlots_[frameIndex];
    if (cuCtxPushCurrent(cudaContext_) != CUDA_SUCCESS) {
        return false;
    }

    // 所有失败分支都需要保证弹出上下文，避免污染调用栈上的CUDA上下文状态
    auto popCudaContextAndFail = []() -> bool {
        CUcontext popped = nullptr;
        cuCtxPopCurrent(&popped);
        return false;
    };

    uint8_t *dstPtr = frame.data(0);
    const int frameWidth = frame.width();
    const int frameHeight = frame.height();
    const int dstLineSize = frame.linesize(0);
    if (!dstPtr || frameWidth <= 0 || frameHeight <= 0 || dstLineSize <= 0 || !slot.cuArrayPtr) {
        return popCudaContextAndFail();
    }

    // 简单考虑，按照RGBA8格式导出
    // 当前离屏图像是RGBA8，导出到编码帧时每行拷贝字节数固定为 width * 4
    const size_t copyWidthInBytes = static_cast<size_t>(frameWidth) * 4;
    if (copyWidthInBytes > static_cast<size_t>(dstLineSize)) {
        return popCudaContextAndFail();
    }

    CUstream stream = cudaStream_;
    if (slot.hasPendingCudaSignal && slot.cudaExtSemaphore) {
        CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS waitParams{};
        CUexternalSemaphore sem = slot.cudaExtSemaphore;
        if (cuWaitExternalSemaphoresAsync(&sem, &waitParams, 1, stream) != CUDA_SUCCESS) {
            return popCudaContextAndFail();
        }
    }

    CUarray array = nullptr;
    if (cuMipmappedArrayGetLevel(&array, slot.cuArrayPtr, 0) != CUDA_SUCCESS || !array) {
        return popCudaContextAndFail();
    }

    CUDA_MEMCPY2D cpy{};
    cpy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
    cpy.srcArray = array;
    cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cpy.dstDevice = reinterpret_cast<CUdeviceptr>(dstPtr);
    cpy.dstPitch = static_cast<size_t>(dstLineSize);
    cpy.WidthInBytes = copyWidthInBytes;
    cpy.Height = static_cast<size_t>(frameHeight);

    if (cuMemcpy2DAsync(&cpy, stream) != CUDA_SUCCESS ||
        cuStreamSynchronize(stream) != CUDA_SUCCESS) {
        return popCudaContextAndFail();
    }

    slot.hasPendingCudaSignal = false;
    CUcontext popped = nullptr;
    cuCtxPopCurrent(&popped);
    return true;
}

bool VulkanRenderer::createFramebuffers()
{
    if (!debug_)
        return true;

    framebuffers_.resize(swapchainImageViews_.size());
    for (size_t i = 0; i < swapchainImageViews_.size(); ++i) {
        // 将交换链图像绑定到对应的帧缓冲对象，生成对应内容，以达到同步修改swapchain image
        VkImageView attachments[] = {swapchainImageViews_[i]};

        // frame buffer
        // renderPass - 指定工作的渲染通道
        // attachmentCount - 绑定的图像附件数量
        // pAttachments - 绑定的图像附件
        // width - 帧缓冲宽度
        // height - 帧缓冲高度
        // layers - 帧缓冲的图层书
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = renderPass_;
        fb.attachmentCount = 1;
        fb.pAttachments = attachments;
        fb.width = swapchain_.extent.width;
        fb.height = swapchain_.extent.height;
        fb.layers = 1;
        if (vkCreateFramebuffer(device_.device, &fb, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
            std::cerr << "Failed to create framebuffer" << std::endl;
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::createCommandPool()
{
    // 创建命令池，每个comman buffer可以单独重置（command buffer有独立的生命周期）
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = graphicsQueueFamilyIndex_;
    if (vkCreateCommandPool(device_.device, &ci, nullptr, &commandPool_) != VK_SUCCESS) {
        std::cerr << "Failed to create command pool" << std::endl;
        return false;
    }

    // 根据最大飞行帧数量，创建命令缓冲区
    commandBuffers_.resize(kMaxFramesInFlight);
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = commandPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kMaxFramesInFlight;
    if (vkAllocateCommandBuffers(device_.device, &ai, commandBuffers_.data()) != VK_SUCCESS) {
        std::cerr << "Failed to allocate command buffers" << std::endl;
        return false;
    }
    return true;
}

bool VulkanRenderer::createSyncObjects()
{
    // 为各类同步对象的容器一次性申请足量内存空间
    imageAvailableSemaphores_.resize(kMaxFramesInFlight);
    inFlightFences_.resize(kMaxFramesInFlight);
    renderFinishedSemaphores_.resize(swapchainImageViews_.size());
    imagesInFlight_.assign(swapchainImageViews_.size(), VK_NULL_HANDLE);

    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    // Fence创建时，以置位状态创建。置位 = 任务已完成（资源已可用）
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    // 为飞行帧创建信号量和栅栏
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(device_.device, &si, nullptr, &imageAvailableSemaphores_[i]) !=
                VK_SUCCESS ||
            vkCreateFence(device_.device, &fi, nullptr, &inFlightFences_[i]) != VK_SUCCESS) {
            std::cerr << "Failed to create sync objects" << std::endl;
            return false;
        }
    }

    // 创建等待GPU渲染完成的相关信号量
    for (size_t i = 0; i < renderFinishedSemaphores_.size(); ++i) {
        if (vkCreateSemaphore(device_.device, &si, nullptr, &renderFinishedSemaphores_[i]) !=
            VK_SUCCESS) {
            std::cerr << "Failed to create per-image renderFinished semaphores" << std::endl;
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::createDescriptorLayouts()
{
    // 创建视频描述符集，用来绑定资源到着色器
    // binding - 着色器中的访问位置 如 layout(set = 0, binding = 0)
    // descriptorType - 描述符资源类型
    // descriptorCount - 描述符数量，对应着色器中对应访问位置的变量数目
    // stageFlags - 描述符使用的阶段
    VkDescriptorSetLayoutBinding videoBindings[2]{};
    videoBindings[0].binding = 0;
    videoBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    videoBindings[0].descriptorCount = 1;
    videoBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    videoBindings[1].binding = 1;
    videoBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    videoBindings[1].descriptorCount = 1;
    videoBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // 创建视频描述符集结构
    // bindingCount - 绑定的描述符集数目
    // pBindings - 绑定的描述符集
    VkDescriptorSetLayoutCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    vci.bindingCount = 2;
    vci.pBindings = videoBindings;

    if (vkCreateDescriptorSetLayout(device_.device, &vci, nullptr, &videoSetLayout_) !=
        VK_SUCCESS) {
        return false;
    }

    // 创建OSD描述符集，用来绑定资源到着色器
    VkDescriptorSetLayoutBinding osdBinding{};
    osdBinding.binding = 0;
    osdBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    osdBinding.descriptorCount = 1;
    osdBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // 创建OSD描述符集结构
    VkDescriptorSetLayoutCreateInfo oci{};
    oci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    oci.bindingCount = 1;
    oci.pBindings = &osdBinding;

    if (vkCreateDescriptorSetLayout(device_.device, &oci, nullptr, &osdSetLayout_) != VK_SUCCESS) {
        return false;
    }

    // 视频和OSD都采用同样的采样模式：最邻近采样 + 边缘钳制
    if (videoSampler_ == VK_NULL_HANDLE) {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxAnisotropy = 1.0f;
        if (vkCreateSampler(device_.device, &sci, nullptr, &videoSampler_) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::createDescriptorPoolAndSets()
{
    // 需要描述符的图像总数：2（飞行帧数量）
    // 图像中所需的描述符：
    // - 视频：Y + UV（2个）
    // - OSD：1个
    // - 调试呈现路径下的Blit：1个
    // 避免发生数据竞争，预留足够的set数量与descriptor数量
    const uint32_t setsPerFrame = debug_ ? 4u : 3u;
    const uint32_t totalSets = kMaxFramesInFlight * setsPerFrame;

    VkDescriptorPoolSize poolSizes[1]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = totalSets;

    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = poolSizes;
    pci.maxSets = totalSets;

    if (vkCreateDescriptorPool(device_.device, &pci, nullptr, &descriptorPool_) != VK_SUCCESS) {
        return false;
    }

    videoDescriptorSets_.resize(kMaxFramesInFlight);
    osdDescriptorSets_.resize(kMaxFramesInFlight);

    std::vector<VkDescriptorSetLayout> videoLayouts(kMaxFramesInFlight, videoSetLayout_);
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = descriptorPool_;
    ai.descriptorSetCount = kMaxFramesInFlight;
    ai.pSetLayouts = videoLayouts.data();
    if (vkAllocateDescriptorSets(device_.device, &ai, videoDescriptorSets_.data()) != VK_SUCCESS) {
        return false;
    }

    std::vector<VkDescriptorSetLayout> osdLayouts(kMaxFramesInFlight, osdSetLayout_);
    ai.pSetLayouts = osdLayouts.data();
    ai.descriptorSetCount = kMaxFramesInFlight;
    if (vkAllocateDescriptorSets(device_.device, &ai, osdDescriptorSets_.data()) != VK_SUCCESS) {
        return false;
    }

    videoDescriptorsValid_.assign(kMaxFramesInFlight, false);
    lastVideoSetIndex_ = 0;
    hasVideoSet_ = false;

    return true;
}

std::vector<uint32_t> VulkanRenderer::readSpvFile(const std::string &path) const
{
    // 打开二进制着色器文件
    std::ifstream file(shaderPath(path), std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {};
    }

    // 读取全部内容，并返回
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return {};
    }
    std::vector<uint32_t> buffer(static_cast<size_t>(size) / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(buffer.data()), size);
    return buffer;
}

std::filesystem::path VulkanRenderer::shaderPath(const std::string &relative) const
{
    // 拼接着色器路径
    if (exeDir_.empty()) {
        return std::filesystem::path(relative);
    }
    return exeDir_ / std::filesystem::path(relative);
}

bool VulkanRenderer::ensureShaderSpv(const std::string &glslRelativePath,
                                     const std::string &spvRelativePath, int shaderKind) const
{
    // 得到着色器文本文件和二进制文件的地址
    const auto spvPath = shaderPath(spvRelativePath);
    const auto glslPath = shaderPath(glslRelativePath);

    // 判断是存在
    const bool spvExists = std::filesystem::exists(spvPath);
    const bool glslExists = std::filesystem::exists(glslPath);

    // 若spv已存在，则比较文本文件是否有更新，无更新则返回
    if (spvExists) {
        if (!glslExists) {
            return true;
        }
        std::error_code ec;
        const auto spvTime = std::filesystem::last_write_time(spvPath, ec);
        const auto glslTime = std::filesystem::last_write_time(glslPath, ec);
        if (!ec && spvTime >= glslTime) {
            return true;
        }
    }

    // 需要重新生成spv，先读取文本文件
    std::string source;
    if (!readTextFile(glslPath, source)) {
        if (spvExists) {
            std::cerr << "[Shader] Failed to read GLSL source (fallback to existing SPV): "
                      << glslPath.string() << std::endl;
            return true;
        }
        std::cerr << "[Shader] Failed to read GLSL source: " << glslPath.string() << std::endl;
        return false;
    }

    // 编译spv，并得到编译结果
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);

    const auto result = compiler.CompileGlslToSpv(
        source, static_cast<shaderc_shader_kind>(shaderKind), glslPath.string().c_str(), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        if (spvExists) {
            std::cerr << "[Shader] Compile failed (fallback to existing SPV): " << glslPath.string()
                      << "\n"
                      << result.GetErrorMessage() << std::endl;
            return true;
        }
        std::cerr << "[Shader] Compile failed: " << glslPath.string() << "\n"
                  << result.GetErrorMessage() << std::endl;
        return false;
    }

    // 将编译结果保存在对应的文件中
    std::vector<uint32_t> spirv(result.cbegin(), result.cend());

    std::filesystem::create_directories(spvPath.parent_path());
    if (!writeBinaryFile(spvPath, spirv)) {
        std::cerr << "[Shader] Failed to write SPIR-V cache: " << spvPath.string() << std::endl;
        return false;
    }

    return true;
}

VkShaderModule VulkanRenderer::createShaderModule(const std::vector<uint32_t> &code) const
{
    // 若传入的着色器代码为空，则返回
    if (code.empty()) {
        return VK_NULL_HANDLE;
    }

    // 着色器模块创建信息
    // codeSize - 代码大小
    // pCode - 代码
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size() * sizeof(uint32_t);
    ci.pCode = code.data();

    // 创建并返回
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_.device, &ci, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

bool VulkanRenderer::createVideoPipeline()
{
    // 确保着色器已动态编译完成
    if (!ensureShaderSpv("shaders/video.vert", "shaders/video.vert.spv",
                         shaderc_glsl_vertex_shader) ||
        !ensureShaderSpv("shaders/video.frag", "shaders/video.frag.spv",
                         shaderc_glsl_fragment_shader)) {
        return false;
    }

    // 加载着色器，并创建着色器模块
    const auto vert = readSpvFile("shaders/video.vert.spv");
    const auto frag = readSpvFile("shaders/video.frag.spv");
    VkShaderModule vertMod = createShaderModule(vert);
    VkShaderModule fragMod = createShaderModule(frag);
    if (vertMod == VK_NULL_HANDLE || fragMod == VK_NULL_HANDLE) {
        std::cerr << "Failed to load video shaders. Make sure shaders are copied next to exe."
                  << std::endl;
        return false;
    }

    // 渲染管线-着色器阶段的相关声明
    // stage - 着色器阶段
    // module - 使用哪个着色器
    // pName - 接入点函数名称（入口函数名）
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName = "main";

    // 渲染管线-顶点输入阶段的绑定声明
    // binding - 指定顶点输入对应顶点缓冲区的绑定位置，用于VkVertexInputAttributeDescription中
    // stride - 每个顶点数据之间的步长间隔，单位为字节
    // inputRate - 顶点数据的读取方式；每次绘制完一个顶点，就读取下一个
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(VideoVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // 渲染管线-顶点输入阶段的属性（使用）声明
    // binding - 顶点数据的下标索引，由VkVertexInputBindingDescription指定
    // location - 在着色器中使用，用来索引顶点数据，值唯一。如layout(location = 0) in vec2 inPos;
    // format - 数据格式，要和着色器中使用时的数据格式保持一致。如 vec2
    // offset - 数据的偏移，交错使用顶点数据时，需要根据偏移取值。如 inPos
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = offsetof(VideoVertex, pos);
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(VideoVertex, uv);

    // 渲染管线-顶点输入状态的创建信息
    // vertexBindingDescriptionCount - 顶点输入数据的绑定数量
    // pVertexBindingDescriptions - 绑定的顶点输入数据
    // vertexAttributeDescriptionCount - 顶点输入属性说明的绑定数量
    // pVertexAttributeDescriptions - 顶点输入属性说明
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    // 渲染管线-装配状态的创建信息
    // topology - 输入的图元拓扑类型，如何使用顶点；
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // 渲染管线-视口裁剪状态的创建信息
    // viewportCount - 视口数量
    // scissorCount - 裁剪范围数量
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    // 渲染管线-栅格化状态的创建信息
    // polygonMode - 三角形绘制模式
    // cullMode - 面剔除模式
    // frontFace - 正面顶点顺序是顺时针还是逆时针
    // lineWidth - 线宽，多数情况下固定为1
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    // 渲染管线-多重采样状态的创建信息
    // rasterizationSamples - 采样点个数
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // 渲染管线-颜色附件的混色方式
    // colorWriteMask - 往哪些通道中写入数据
    // blendEnable - 是否开启混色
    VkPipelineColorBlendAttachmentState cbAttach{};
    cbAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cbAttach.blendEnable = VK_FALSE;

    // 渲染管线-颜色混合状态的创建信息
    // attachmentCount - 颜色附件的数量
    // pAttachments - 绑定的颜色巨剑
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cbAttach;

    // 渲染管线-可动态更新状态的创建信息
    // dynamicStateCount - 可动态更新状态数量
    // pDynamicStates - 有哪些状态可动态更新
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    // 渲染管线-执行过程中，可由CPU推送的变更数据
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = sizeof(int) * 4;

    // 渲染管线的结构定义
    // setLayoutCount - 使用的资源描述符结构数量
    // pSetLayouts - 使用的资源描述符结构
    // pushConstantRangeCount - 使用的推送变更数据数量
    // pPushConstantRanges - 使用的推送变更数据
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &videoSetLayout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(device_.device, &pli, nullptr, &videoPipelineLayout_) !=
        VK_SUCCESS) {
        return false;
    }

    // 渲染管线的创建信息
    // stageCount - 着色器阶段数量
    // pStages - 着色器阶段定义
    // pVertexInputState - 顶点输入状态
    // pInputAssemblyState - 状态输入状态
    // pViewportState - 视口状态
    // pRasterizationState - 栅格化状态
    // pMultisampleState - 多重采样状态
    // pColorBlendState - 颜色混合状态
    // pDynamicState - 动态修改状态
    // layout - 结构定义
    // renderPass - 渲染通道
    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = videoPipelineLayout_;
    gp.renderPass = offscreenRenderPass_;
    gp.subpass = 0;

    // 创建管线，并销毁着色器
    const VkResult res =
        vkCreateGraphicsPipelines(device_.device, VK_NULL_HANDLE, 1, &gp, nullptr, &videoPipeline_);
    vkDestroyShaderModule(device_.device, fragMod, nullptr);
    vkDestroyShaderModule(device_.device, vertMod, nullptr);
    if (res != VK_SUCCESS) {
        std::cerr << "Failed to create video pipeline" << std::endl;
        return false;
    }
    return true;
}

bool VulkanRenderer::createOsdPipeline()
{
    if (!ensureShaderSpv("shaders/osd.vert", "shaders/osd.vert.spv", shaderc_glsl_vertex_shader) ||
        !ensureShaderSpv("shaders/osd.frag", "shaders/osd.frag.spv",
                         shaderc_glsl_fragment_shader)) {
        return false;
    }

    const auto vert = readSpvFile("shaders/osd.vert.spv");
    const auto frag = readSpvFile("shaders/osd.frag.spv");
    VkShaderModule vertMod = createShaderModule(vert);
    VkShaderModule fragMod = createShaderModule(frag);
    if (vertMod == VK_NULL_HANDLE || fragMod == VK_NULL_HANDLE) {
        std::cerr << "Failed to load OSD shaders. Make sure shaders are copied next to exe."
                  << std::endl;
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(OsdVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = offsetof(OsdVertex, posPx);
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(OsdVertex, uv);
    attrs[2].binding = 0;
    attrs[2].location = 2;
    attrs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[2].offset = offsetof(OsdVertex, color);

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cbAttach{};
    cbAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cbAttach.blendEnable = VK_TRUE;
    cbAttach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cbAttach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cbAttach.colorBlendOp = VK_BLEND_OP_ADD;
    cbAttach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbAttach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cbAttach.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cbAttach;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc.offset = 0;
    pc.size = sizeof(float) * 2;

    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &osdSetLayout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(device_.device, &pli, nullptr, &osdPipelineLayout_) != VK_SUCCESS) {
        return false;
    }

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = osdPipelineLayout_;
    gp.renderPass = offscreenRenderPass_;
    gp.subpass = 0;

    const VkResult res =
        vkCreateGraphicsPipelines(device_.device, VK_NULL_HANDLE, 1, &gp, nullptr, &osdPipeline_);
    vkDestroyShaderModule(device_.device, fragMod, nullptr);
    vkDestroyShaderModule(device_.device, vertMod, nullptr);
    if (res != VK_SUCCESS) {
        std::cerr << "Failed to create OSD pipeline" << std::endl;
        return false;
    }
    return true;
}

bool VulkanRenderer::createBlitPipeline()
{
    if (!debug_)
        return true;

    // ========== 创建Blit描述符集布局 ==========
    // 用于绑定离屏纹理到着色器
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    if (vkCreateDescriptorSetLayout(device_.device, &layoutInfo, nullptr, &blitSetLayout_) !=
        VK_SUCCESS) {
        std::cerr << "Failed to create blit descriptor set layout" << std::endl;
        return false;
    }

    // ========== 分配Blit描述符集 ==========
    blitDescriptorSets_.resize(kMaxFramesInFlight);
    std::vector<VkDescriptorSetLayout> layouts(kMaxFramesInFlight, blitSetLayout_);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = kMaxFramesInFlight;
    allocInfo.pSetLayouts = layouts.data();

    if (vkAllocateDescriptorSets(device_.device, &allocInfo, blitDescriptorSets_.data()) !=
        VK_SUCCESS) {
        std::cerr << "Failed to allocate blit descriptor sets" << std::endl;
        return false;
    }

    // 更新描述符集，绑定离屏纹理
    for (size_t i = 0; i < blitDescriptorSets_.size(); ++i) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = offscreenSampler_;
        imageInfo.imageView = vulkanSlots_[i].view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = blitDescriptorSets_[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(device_.device, 1, &write, 0, nullptr);
    }

    // ========== 创建简单的Blit着色器 ==========
    // 由于没有现成的着色器文件，这里使用内联的SPIR-V代码或简化处理
    // 实际应该使用预编译的SPIR-V，这里创建一个简单的管线布局
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &blitSetLayout_;

    if (vkCreatePipelineLayout(device_.device, &pipelineLayoutInfo, nullptr,
                               &blitPipelineLayout_) != VK_SUCCESS) {
        std::cerr << "Failed to create blit pipeline layout" << std::endl;
        return false;
    }

    // ========== 创建Blit顶点缓冲（全屏四边形）==========
    struct BlitVertex {
        float pos[2];
        float uv[2];
    };

    // 全屏四边形顶点（两个三角形）
    // Vulkan的纹理坐标Y轴与NDC坐标相反，需要反转以显示正确的图像
    const BlitVertex blitQuad[6] = {
        {{-1.0f, -1.0f}, {0.0f, 0.0f}}, // 左下 -> UV (0,0)
        {{1.0f, -1.0f}, {1.0f, 0.0f}},  // 右下 -> UV (1,0)
        {{1.0f, 1.0f}, {1.0f, 1.0f}},   // 右上 -> UV (1,1)
        {{-1.0f, -1.0f}, {0.0f, 0.0f}}, // 左下 -> UV (0,0)
        {{1.0f, 1.0f}, {1.0f, 1.0f}},   // 右上 -> UV (1,1)
        {{-1.0f, 1.0f}, {0.0f, 1.0f}},  // 左上 -> UV (0,1)
    };

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeof(blitQuad);
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCreateInfo{};
    allocCreateInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;

    if (vmaCreateBuffer(allocator_, &bufferInfo, &allocCreateInfo, &blitVertexBuffer_,
                        &blitVertexAlloc_, nullptr) != VK_SUCCESS) {
        std::cerr << "Failed to create blit vertex buffer" << std::endl;
        return false;
    }

    // 填充顶点数据
    void *data;
    vmaMapMemory(allocator_, blitVertexAlloc_, &data);
    std::memcpy(data, blitQuad, sizeof(blitQuad));
    vmaUnmapMemory(allocator_, blitVertexAlloc_);

    // ========== 编译Blit着色器到SPIR-V ==========
    if (!ensureShaderSpv("shaders/blit.vert", "shaders/blit.vert.spv",
                         shaderc_glsl_vertex_shader) ||
        !ensureShaderSpv("shaders/blit.frag", "shaders/blit.frag.spv",
                         shaderc_glsl_fragment_shader)) {
        std::cerr << "Failed to compile blit shaders to SPIR-V" << std::endl;
        return false;
    }

    // 加载编译好的着色器，并创建着色器模块
    const auto vert = readSpvFile("shaders/blit.vert.spv");
    const auto frag = readSpvFile("shaders/blit.frag.spv");
    VkShaderModule vertMod = createShaderModule(vert);
    VkShaderModule fragMod = createShaderModule(frag);
    if (vertMod == VK_NULL_HANDLE || fragMod == VK_NULL_HANDLE) {
        std::cerr << "Failed to load blit shaders. Make sure shaders are copied next to exe."
                  << std::endl;
        return false;
    }

    // ========== 创建Blit图形管线 ==========
    // 着色器阶段定义
    // stage - 着色器阶段
    // module - 使用的着色器模块
    // pName - 着色器入口函数名
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName = "main";

    // 顶点输入绑定描述
    // binding - 顶点缓冲区绑定位置
    // stride - 每个顶点数据的字节步长
    // inputRate - 顶点数据读取方式（每个顶点读取一次）
    VkVertexInputBindingDescription vertBinding{};
    vertBinding.binding = 0;
    vertBinding.stride = sizeof(BlitVertex);
    vertBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // 顶点输入属性描述
    // location - 着色器中的输入位置
    // binding - 对应的缓冲区绑定
    // format - 属性数据格式
    // offset - 在顶点数据中的偏移
    VkVertexInputAttributeDescription vertAttrs[2]{};
    vertAttrs[0].location = 0;
    vertAttrs[0].binding = 0;
    vertAttrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    vertAttrs[0].offset = offsetof(BlitVertex, pos);
    vertAttrs[1].location = 1;
    vertAttrs[1].binding = 0;
    vertAttrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    vertAttrs[1].offset = offsetof(BlitVertex, uv);

    // 顶点输入状态
    VkPipelineVertexInputStateCreateInfo vertInput{};
    vertInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertInput.vertexBindingDescriptionCount = 1;
    vertInput.pVertexBindingDescriptions = &vertBinding;
    vertInput.vertexAttributeDescriptionCount = 2;
    vertInput.pVertexAttributeDescriptions = vertAttrs;

    // 输入装配状态（三角形列表）
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // 视口和裁剪状态
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // 栅格化状态
    // polygonMode - 三角形绘制模式（填充）
    // cullMode - 面剔除（不剔除）
    // frontFace - 正面顶点顺序（顺时针）
    // lineWidth - 线宽（固定为1）
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    // 多重采样状态
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // 颜色混合附件状态（禁用混合，直接覆盖）
    VkPipelineColorBlendAttachmentState colorBlendAttach{};
    colorBlendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttach.blendEnable = VK_FALSE;

    // 颜色混合状态
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &colorBlendAttach;

    // 动态状态（视口和裁剪区域在渲染时动态设置）
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynStates;

    // 创建完整的图形管线
    // stageCount - 着色器阶段数量
    // pStages - 着色器阶段数组
    // pVertexInputState - 顶点输入状态
    // pInputAssemblyState - 输入装配状态
    // pViewportState - 视口状态
    // pRasterizationState - 栅格化状态
    // pMultisampleState - 多重采样状态
    // pColorBlendState - 颜色混合状态
    // pDynamicState - 动态状态
    // layout - 管线布局
    // renderPass - 渲染通道
    // subpass - 子通道索引
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = blitPipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    // 创建管线并销毁着色器模块
    const VkResult pipelineRes = vkCreateGraphicsPipelines(device_.device, VK_NULL_HANDLE, 1,
                                                           &pipelineInfo, nullptr, &blitPipeline_);
    vkDestroyShaderModule(device_.device, fragMod, nullptr);
    vkDestroyShaderModule(device_.device, vertMod, nullptr);

    if (pipelineRes != VK_SUCCESS) {
        std::cerr << "Failed to create blit pipeline" << std::endl;
        return false;
    }

    return true;
}

bool VulkanRenderer::createVideoGeometry()
{
    // 创建视频区域的顶点坐标。绘制两个三角形
    const VideoVertex verts[6] = {
        {{-1.0f, -1.0f}, {0.0f, 0.0f}}, {{1.0f, -1.0f}, {1.0f, 0.0f}},
        {{1.0f, 1.0f}, {1.0f, 1.0f}},   {{-1.0f, -1.0f}, {0.0f, 0.0f}},
        {{1.0f, 1.0f}, {1.0f, 1.0f}},   {{-1.0f, 1.0f}, {0.0f, 1.0f}},
    };

    // 创建顶点缓冲区
    // size - 缓冲区大小
    // usage - 缓冲区的用途
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = sizeof(verts);
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    // 内存分配信息，映射内存数据
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    // 创建缓冲区
    VmaAllocationInfo ai{};
    if (vmaCreateBuffer(allocator_, &bi, &aci, &videoVertexBuffer_, &videoVertexAlloc_, &ai) !=
        VK_SUCCESS) {
        return false;
    }

    // 拷贝数据
    if (ai.pMappedData) {
        std::memcpy(ai.pMappedData, verts, sizeof(verts));
    }
    vmaFlushAllocation(allocator_, videoVertexAlloc_, 0, VK_WHOLE_SIZE);
    return true;
}

bool VulkanRenderer::createOsdGeometry()
{
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = sizeof(OsdVertex) * 6;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    // 创建缓冲区后，保留CPU的映射地址，便于修改
    VmaAllocationInfo ai{};
    if (vmaCreateBuffer(allocator_, &bi, &aci, &osdVertexBuffer_, &osdVertexAlloc_, &ai) !=
        VK_SUCCESS) {
        return false;
    }
    osdVertexMapped_ = ai.pMappedData;
    return true;
}

void VulkanRenderer::destroySwapchainObjects()
{
    // 销毁帧缓冲区对象
    for (auto fb : framebuffers_) {
        if (fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_.device, fb, nullptr);
        }
    }
    framebuffers_.clear();

    // 销毁交换链图像的视图
    for (auto v : swapchainImageViews_) {
        if (v != VK_NULL_HANDLE) {
            vkDestroyImageView(device_.device, v, nullptr);
        }
    }
    swapchainImageViews_.clear();

    // 销毁交换链
    if (swapchain_.swapchain != VK_NULL_HANDLE) {
        vkb::destroy_swapchain(swapchain_);
        swapchain_ = {};
    }
}

bool VulkanRenderer::recreateSwapchain()
{
    if (!debug_) {
        windowResized_ = false;
        return true;
    }

    if (!window_ || !device_.device) {
        return false;
    }

    // 读取当前窗口（纹理缓冲对象）的宽高
    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    while (w == 0 || h == 0) {
        glfwWaitEvents();
        glfwGetFramebufferSize(window_, &w, &h);
    }
    // 若仍然宽高为0，则重建失败
    if (w == 0 || h == 0)
        return false;

    // 等待设备空闲
    vkDeviceWaitIdle(device_.device);
    // 销毁交换链对象
    destroySwapchainObjects();

    // 创建交换链和相关的纹理缓冲区
    if (!createSwapchain())
        return false;
    if (!createFramebuffers())
        return false;

    // 销毁并重建描述符资源池
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_.device, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    if (!createDescriptorPoolAndSets()) {
        return false;
    }

    // 重建Blit描述符集。
    // 注意：如果Blit管线还未创建（例如首次绘制前就触发了窗口缩放），这里先尝试创建管线。
    if (blitPipeline_ == VK_NULL_HANDLE || blitPipelineLayout_ == VK_NULL_HANDLE ||
        blitSetLayout_ == VK_NULL_HANDLE) {
        if (!createBlitPipeline()) {
            return false;
        }
    } else {
        if (offscreenSampler_ == VK_NULL_HANDLE || vulkanSlots_.size() < kMaxFramesInFlight) {
            return false;
        }

        blitDescriptorSets_.assign(kMaxFramesInFlight, VK_NULL_HANDLE);
        std::vector<VkDescriptorSetLayout> blitLayouts(kMaxFramesInFlight, blitSetLayout_);
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool_;
        allocInfo.descriptorSetCount = kMaxFramesInFlight;
        allocInfo.pSetLayouts = blitLayouts.data();
        if (vkAllocateDescriptorSets(device_.device, &allocInfo, blitDescriptorSets_.data()) !=
            VK_SUCCESS) {
            std::cerr << "Failed to allocate blit descriptor sets (recreateSwapchain)" << std::endl;
            return false;
        }

        // 更新Blit描述符集，绑定重建后的离屏纹理
        for (size_t i = 0; i < blitDescriptorSets_.size(); ++i) {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.sampler = offscreenSampler_;
            imageInfo.imageView = vulkanSlots_[i].view;
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = blitDescriptorSets_[i];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfo;

            vkUpdateDescriptorSets(device_.device, 1, &write, 0, nullptr);
        }
    }

    // 如果osdImageView和osdSampler不为空，则同步修改描述符资源，绑定采样器
    if (osdImageView_ != VK_NULL_HANDLE && osdSampler_ != VK_NULL_HANDLE) {
        for (size_t i = 0; i < osdDescriptorSets_.size(); ++i) {
            VkDescriptorImageInfo di{};
            di.sampler = osdSampler_;
            di.imageView = osdImageView_;
            di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = osdDescriptorSets_[i];
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &di;
            vkUpdateDescriptorSets(device_.device, 1, &w, 0, nullptr);
        }
    }

    // 销毁同步信号
    for (auto sem : renderFinishedSemaphores_) {
        if (sem != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_.device, sem, nullptr);
        }
    }
    renderFinishedSemaphores_.clear();
    imagesInFlight_.clear();

    // 重建同步信号
    renderFinishedSemaphores_.resize(swapchainImageViews_.size());
    imagesInFlight_.assign(swapchainImageViews_.size(), VK_NULL_HANDLE);
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (size_t i = 0; i < renderFinishedSemaphores_.size(); ++i) {
        if (vkCreateSemaphore(device_.device, &si, nullptr, &renderFinishedSemaphores_[i]) !=
            VK_SUCCESS) {
            std::cerr << "Failed to create per-image renderFinished semaphores (recreateSwapchain)"
                      << std::endl;
            for (size_t j = 0; j < i; ++j) {
                if (renderFinishedSemaphores_[j] != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device_.device, renderFinishedSemaphores_[j], nullptr);
                    renderFinishedSemaphores_[j] = VK_NULL_HANDLE;
                }
            }
            renderFinishedSemaphores_.clear();
            imagesInFlight_.clear();
            return false;
        }
    }

    windowResized_ = false;
    return true;
}

void VulkanRenderer::ensureStagingBuffer(VkDeviceSize requiredSize, VkBuffer &buffer,
                                         VmaAllocation &alloc, void *&mapped,
                                         VkBufferUsageFlags usage)
{
    // 得到已分配的大小
    VkDeviceSize *trackedSize = nullptr;
    if (&buffer == &osdStagingBuffer_) {
        trackedSize = &osdStagingSize_;
    }

    // 如果已经分配，且分配的值大于请求值，则不用重新分配
    if (trackedSize && buffer != VK_NULL_HANDLE && *trackedSize >= requiredSize) {
        return;
    }

    // 销毁之前的缓冲区
    if (buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer, alloc);
        buffer = VK_NULL_HANDLE;
        alloc = VK_NULL_HANDLE;
        mapped = nullptr;
    }

    // 创建新的缓冲区
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = requiredSize;
    bi.usage = usage;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo ai{};
    if (vmaCreateBuffer(allocator_, &bi, &aci, &buffer, &alloc, &ai) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create staging buffer");
    }

    // 保存CPU映射的地址；更新已分配缓冲区大小
    mapped = ai.pMappedData;
    if (trackedSize) {
        *trackedSize = requiredSize;
    }
}

void VulkanRenderer::transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                                     VkImageLayout newLayout, VkPipelineStageFlags srcStage,
                                     VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                                     VkAccessFlags dstAccess, VkImageAspectFlags aspectMask) const
{
    // 为图像进行layout和access之间的转换
    // 便于下一阶段进行使用
    // srcAccessMask - 源阶段进行的操作
    // dstAccessMask - 目标阶段进行的操作
    // oldLayout - 旧的的使用方式
    // newLayout - 新的使用方式
    // srcQueueFamilyIndex - 源阶段队列族索引
    // dstQueueFamilyIndex - 目标阶段队列族索引
    // image - 进行转换的图像
    // subresourceRange - 对图像资源的访问范围
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    // 进行转换
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void VulkanRenderer::copyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer, VkImage image,
                                       uint32_t width, uint32_t height,
                                       VkImageAspectFlags aspectMask) const
{
    // 将VkBuffer中的内容，拷贝到VkImage中
    // bufferOffset - 拷贝起始位置（字节）
    // bufferRowLength - 一行 texel 的宽度（以 texel 为单位，texel 数量）
    // bufferImageHeight - 一张 2D slice 的高度（texel）
    // imageSubresource - 写入image的哪个部分
    // imageOffset - image 内的起始写入位置
    // imageExtent - 拷贝区域大小（宽、高、深度）
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = aspectMask;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void VulkanRenderer::setCudaContext(CUcontext ctx)
{
    cudaContext_ = ctx;
    if (initialized_) {
        initCudaInterop();
    }
}

uint32_t VulkanRenderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const
{
    // 找到适合的内存类型
    // 遍历当前所有的内存类型，找到符合类型且支持相关属性的内存索引
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_.physical_device, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0 &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

bool VulkanRenderer::createExportableImage(uint32_t width, uint32_t height, VkFormat format,
                                           VkImage &image, VkDeviceMemory &memory)
{
    // 导出图片的创建信息
    // handleTypes - 导出类型
    VkExternalMemoryImageCreateInfo extImage{};
    extImage.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extImage.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    // 图片的创建信息
    // pNext - 附加信息，用来表明这个缓冲区是导出的
    // size - 数据大小
    // usage - 用途，存储的数据类型
    // sharingMode - 共享方式
    VkImageCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    vi.pNext = &extImage;
    vi.imageType = VK_IMAGE_TYPE_2D;
    vi.format = format;
    vi.extent = {width, height, 1};
    vi.mipLevels = 1;
    vi.arrayLayers = 1;
    vi.samples = VK_SAMPLE_COUNT_1_BIT;
    vi.tiling = VK_IMAGE_TILING_OPTIMAL;
    vi.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    vi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_.device, &vi, nullptr, &image) != VK_SUCCESS) {
        return false;
    }

    // 确定内存需求
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_.device, image, &req);

    // 导出内存的分配信息
    // handleTypes - 导出类型
    VkExportMemoryAllocateInfo exportAlloc{};
    exportAlloc.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportAlloc.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    // 内存分配信息
    // pNext - 附加信息
    // allocationSize - 需要分配的大小
    // memoryTypeIndex - 内存类型的索引
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &exportAlloc;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // 分配内存
    if (vkAllocateMemory(device_.device, &mai, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyImage(device_.device, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }

    // 缓冲区和实际内存区域进行绑定
    if (vkBindImageMemory(device_.device, image, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device_.device, memory, nullptr);
        vkDestroyImage(device_.device, image, nullptr);
        memory = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool VulkanRenderer::exportMemoryHandle(VkDeviceMemory memory, HANDLE &handle) const
{
    // 得到导出函数
    auto fn = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
        vkGetDeviceProcAddr(device_.device, "vkGetMemoryWin32HandleKHR"));
    if (!fn) {
        return false;
    }

    // 导出信息
    // memory - 内存
    // handleType - 导出类型
    VkMemoryGetWin32HandleInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    info.memory = memory;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    return fn(device_.device, &info, &handle) == VK_SUCCESS;
}

bool VulkanRenderer::exportSemaphoreHandle(VkSemaphore semaphore, HANDLE &handle) const
{
    // 得到导出函数
    auto fn = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
        vkGetDeviceProcAddr(device_.device, "vkGetSemaphoreWin32HandleKHR"));
    if (!fn) {
        return false;
    }

    // 导出信息
    // semaphore - 信号量
    // handleType - 导出类型
    VkSemaphoreGetWin32HandleInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
    info.semaphore = semaphore;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    return fn(device_.device, &info, &handle) == VK_SUCCESS;
}

void VulkanRenderer::destroyOffscreenResources()
{
    // 销毁Blit管线相关资源
    if (blitVertexBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, blitVertexBuffer_, blitVertexAlloc_);
        blitVertexBuffer_ = VK_NULL_HANDLE;
        blitVertexAlloc_ = VK_NULL_HANDLE;
    }

    if (blitPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_.device, blitPipeline_, nullptr);
        blitPipeline_ = VK_NULL_HANDLE;
    }

    if (blitPipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_.device, blitPipelineLayout_, nullptr);
        blitPipelineLayout_ = VK_NULL_HANDLE;
    }

    if (blitSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.device, blitSetLayout_, nullptr);
        blitSetLayout_ = VK_NULL_HANDLE;
    }

    blitDescriptorSets_.clear();

    // 销毁离屏纹理采样器
    if (offscreenSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_.device, offscreenSampler_, nullptr);
        offscreenSampler_ = VK_NULL_HANDLE;
    }

    // 销毁离屏渲染通道
    if (offscreenRenderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_.device, offscreenRenderPass_, nullptr);
        offscreenRenderPass_ = VK_NULL_HANDLE;
    }
}

bool VulkanRenderer::initCudaInterop()
{
    if (!cudaContext_ || !device_.device) {
        return false;
    }

    // 推荐的互操作资源数量，和飞行帧相等
    const size_t desiredSlotCount = static_cast<size_t>(kMaxFramesInFlight);
    if (desiredSlotCount == 0) {
        return false;
    }

    // 如果之前的互操作资源非空，并且和当前资源数量不相等，则先删除
    if (!cudaSlots_.empty() && cudaSlots_.size() != desiredSlotCount) {
        shutdownCudaInterop();
    }

    // 创建CUDA工作流，并分配空间
    if (cudaStream_) {
        cudaSlots_.resize(desiredSlotCount);
    } else {
        if (cuCtxPushCurrent(cudaContext_) != CUDA_SUCCESS) {
            return false;
        }

        const CUresult streamRes = cuStreamCreate(&cudaStream_, CU_STREAM_NON_BLOCKING);

        CUcontext popped = nullptr;
        cuCtxPopCurrent(&popped);

        if (streamRes != CUDA_SUCCESS) {
            cudaStream_ = nullptr;
            return false;
        }

        cudaSlots_.clear();
        cudaSlots_.resize(desiredSlotCount);
    }

    // 创建互操作资源
    for (auto &slot : cudaSlots_) {
        // 共享信号量不存在，则创建
        if (slot.cudaReadySemaphore == VK_NULL_HANDLE) {
            // 创建CUDA数据写入成功的信号量，由vulkan导出给CUDA使用
            VkExportSemaphoreCreateInfo exportSem{};
            exportSem.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
            exportSem.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

            VkSemaphoreCreateInfo sci{};
            sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            sci.pNext = &exportSem;
            if (vkCreateSemaphore(device_.device, &sci, nullptr, &slot.cudaReadySemaphore) !=
                VK_SUCCESS) {
                return false;
            }
        }

        // 导出信号量不存在则创建
        if (!slot.cudaExtSemaphore) {
            // 导出信号量
            HANDLE semHandle = nullptr;
            if (!exportSemaphoreHandle(slot.cudaReadySemaphore, semHandle) || !semHandle) {
                return false;
            }

            // 当前上下文设置不成功时，需要关闭句柄
            if (cuCtxPushCurrent(cudaContext_) != CUDA_SUCCESS) {
                CloseHandle(semHandle);
                return false;
            }

            // CUDA外部导出信号量的声明
            // type - 类型
            // handle - 句柄
            CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC desc{};
            desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32;
            desc.handle.win32.handle = semHandle;

            // 导入外部信号量
            // 弹出上下文，并关闭句柄
            const CUresult r = cuImportExternalSemaphore(&slot.cudaExtSemaphore, &desc);
            CUcontext popped2 = nullptr;
            cuCtxPopCurrent(&popped2);
            CloseHandle(semHandle);

            if (r != CUDA_SUCCESS) {
                slot.cudaExtSemaphore = nullptr;
                return false;
            }
        }
    }

    return true;
}

void VulkanRenderer::shutdownCudaInterop()
{
    // 如果当前有设备，则等待设备任务完成
    if (device_.device) {
        vkDeviceWaitIdle(device_.device);
    }

    // 设置cuda当前上下文
    if (cudaContext_) {
        cuCtxPushCurrent(cudaContext_);
    }

    // 销毁各类互操作资源
    for (auto &slot : cudaSlots_) {
        if (slot.yView != VK_NULL_HANDLE) {
            vkDestroyImageView(device_.device, slot.yView, nullptr);
            slot.yView = VK_NULL_HANDLE;
        }
        if (slot.uvView != VK_NULL_HANDLE) {
            vkDestroyImageView(device_.device, slot.uvView, nullptr);
            slot.uvView = VK_NULL_HANDLE;
        }
        if (slot.yImage != VK_NULL_HANDLE) {
            vkDestroyImage(device_.device, slot.yImage, nullptr);
            slot.yImage = VK_NULL_HANDLE;
            slot.yLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        if (slot.uvImage != VK_NULL_HANDLE) {
            vkDestroyImage(device_.device, slot.uvImage, nullptr);
            slot.uvImage = VK_NULL_HANDLE;
            slot.uvLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        if (slot.yMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device_.device, slot.yMemory, nullptr);
            slot.yMemory = VK_NULL_HANDLE;
        }
        if (slot.uvMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device_.device, slot.uvMemory, nullptr);
            slot.uvMemory = VK_NULL_HANDLE;
        }
        if (slot.cudaReadySemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_.device, slot.cudaReadySemaphore, nullptr);
            slot.cudaReadySemaphore = VK_NULL_HANDLE;
        }

        if (slot.yExtMem) {
            cuDestroyExternalMemory(slot.yExtMem);
            slot.yExtMem = nullptr;
            slot.yPtr = 0;
        }
        if (slot.uvExtMem) {
            cuDestroyExternalMemory(slot.uvExtMem);
            slot.uvExtMem = nullptr;
            slot.uvPtr = 0;
        }
        if (slot.cudaExtSemaphore) {
            cuDestroyExternalSemaphore(slot.cudaExtSemaphore);
            slot.cudaExtSemaphore = nullptr;
        }
        slot.width = 0;
        slot.height = 0;
        slot.strideY = 0;
        slot.strideUV = 0;
    }

    // 销毁各类互操作资源
    for (auto &slot : vulkanSlots_) {
        if (slot.framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_.device, slot.framebuffer, nullptr);
            slot.framebuffer = VK_NULL_HANDLE;
        }
        if (slot.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_.device, slot.view, nullptr);
            slot.view = VK_NULL_HANDLE;
        }
        if (slot.image != VK_NULL_HANDLE) {
            vkDestroyImage(device_.device, slot.image, nullptr);
            slot.image = VK_NULL_HANDLE;
            slot.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        if (slot.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_.device, slot.memory, nullptr);
            slot.memory = VK_NULL_HANDLE;
        }
        if (slot.cudaReadySemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_.device, slot.cudaReadySemaphore, nullptr);
            slot.cudaReadySemaphore = VK_NULL_HANDLE;
        }

        if (slot.extMem) {
            cuDestroyExternalMemory(slot.extMem);
            slot.extMem = nullptr;
            slot.cuArrayPtr = 0;
        }
        if (slot.cudaExtSemaphore) {
            cuDestroyExternalSemaphore(slot.cudaExtSemaphore);
            slot.cudaExtSemaphore = nullptr;
        }
        slot.width = 0;
        slot.height = 0;
    }
    vulkanSlots_.clear();

    // 销毁cuda工作流
    if (cudaStream_) {
        cuStreamDestroy(cudaStream_);
        cudaStream_ = nullptr;
    }

    // 弹出cuda上下文
    if (cudaContext_) {
        CUcontext popped = nullptr;
        cuCtxPopCurrent(&popped);
    }

    // 重置状态
    cudaSlots_.clear();
    pendingCudaFrame_ = {};
    cudaFramePending_ = false;
}

bool VulkanRenderer::ensureCudaInteropSlot(CudaInteropSlot &slot, uint32_t width, uint32_t height,
                                           int strideY, int strideUV)
{
    if (!cudaContext_ || !device_.device) {
        return false;
    }

    // 如果各项参数都一致，不需要重新申请
    if (slot.width == width && slot.height == height && slot.strideY == strideY &&
        slot.strideUV == strideUV && slot.yImage != VK_NULL_HANDLE &&
        slot.uvImage != VK_NULL_HANDLE && slot.yView != VK_NULL_HANDLE &&
        slot.uvView != VK_NULL_HANDLE && slot.yExtMem && slot.uvExtMem && slot.yPtr && slot.uvPtr &&
        slot.yLayout == VK_IMAGE_LAYOUT_GENERAL && slot.uvLayout == VK_IMAGE_LAYOUT_GENERAL) {
        return true;
    }

    // 等待设备空闲
    vkDeviceWaitIdle(device_.device);

    if (cuCtxPushCurrent(cudaContext_) != CUDA_SUCCESS) {
        return false;
    }

    // 如果之前的互操作资源已存在，则删除
    if (slot.yExtMem) {
        cuDestroyExternalMemory(slot.yExtMem);
        slot.yExtMem = nullptr;
        slot.yPtr = 0;
    }
    if (slot.uvExtMem) {
        cuDestroyExternalMemory(slot.uvExtMem);
        slot.uvExtMem = nullptr;
        slot.uvPtr = 0;
    }

    // 销毁之前申请的Vulkan相关资源
    if (slot.yView != VK_NULL_HANDLE) {
        vkDestroyImageView(device_.device, slot.yView, nullptr);
        slot.yView = VK_NULL_HANDLE;
    }
    if (slot.uvView != VK_NULL_HANDLE) {
        vkDestroyImageView(device_.device, slot.uvView, nullptr);
        slot.uvView = VK_NULL_HANDLE;
    }
    if (slot.yImage != VK_NULL_HANDLE) {
        vkDestroyImage(device_.device, slot.yImage, nullptr);
        slot.yImage = VK_NULL_HANDLE;
        slot.yLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    if (slot.uvImage != VK_NULL_HANDLE) {
        vkDestroyImage(device_.device, slot.uvImage, nullptr);
        slot.uvImage = VK_NULL_HANDLE;
        slot.uvLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    if (slot.yMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device_.device, slot.yMemory, nullptr);
        slot.yMemory = VK_NULL_HANDLE;
    }
    if (slot.uvMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device_.device, slot.uvMemory, nullptr);
        slot.uvMemory = VK_NULL_HANDLE;
    }

    // Y、UV图像对应的一维数组长度
    const VkDeviceSize ySize = static_cast<VkDeviceSize>(strideY) * height;
    const VkDeviceSize uvSize = static_cast<VkDeviceSize>(strideUV) * ((height + 1) / 2);

    // 创建导出的数据缓冲区，用来让CUDA映射
    if (!createExportableImage(width, height, VK_FORMAT_R8_UNORM, slot.yImage, slot.yMemory)) {
        CUcontext popped = nullptr;
        cuCtxPopCurrent(&popped);
        return false;
    }
    if (!createExportableImage(width / 2, height / 2, VK_FORMAT_R8G8_UNORM, slot.uvImage,
                               slot.uvMemory)) {
        CUcontext popped = nullptr;
        cuCtxPopCurrent(&popped);
        return false;
    }

    // 将图像按需转换为 GENERAL 布局
    // 和 OSD 一样，仅当当前布局不是目标布局时才做转换
    auto transitionImageToGeneral = [&](VkImage image, VkImageLayout &layout) -> bool {
        if (image == VK_NULL_HANDLE || commandPool_ == VK_NULL_HANDLE ||
            graphicsQueue_ == VK_NULL_HANDLE) {
            return false;
        }
        if (layout == VK_IMAGE_LAYOUT_GENERAL) {
            return true;
        }

        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = commandPool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device_.device, &cai, &cmd) != VK_SUCCESS) {
            return false;
        }

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_.device, commandPool_, 1, &cmd);
            return false;
        }

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);

        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_.device, commandPool_, 1, &cmd);
            return false;
        }

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        const VkResult submitRes = vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE);
        const VkResult waitRes =
            (submitRes == VK_SUCCESS) ? vkQueueWaitIdle(graphicsQueue_) : submitRes;
        vkFreeCommandBuffers(device_.device, commandPool_, 1, &cmd);
        if (submitRes == VK_SUCCESS && waitRes == VK_SUCCESS) {
            layout = VK_IMAGE_LAYOUT_GENERAL;
            return true;
        }
        return false;
    };
    if (!transitionImageToGeneral(slot.yImage, slot.yLayout) ||
        !transitionImageToGeneral(slot.uvImage, slot.uvLayout)) {
        CUcontext popped = nullptr;
        cuCtxPopCurrent(&popped);
        return false;
    }

    // 创建y缓冲区视图，声明如何操作缓冲区数据
    // buffer - 对应的缓冲区
    // format - 按照什么数据格式进行操作（读/写）
    // offset - 偏移
    // range - 可操作区域
    VkImageViewCreateInfo yv{};
    yv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    yv.image = slot.yImage;
    yv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    yv.format = VK_FORMAT_R8_UNORM;
    yv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    yv.subresourceRange.baseArrayLayer = 0;
    yv.subresourceRange.layerCount = 1;
    yv.subresourceRange.baseMipLevel = 0;
    yv.subresourceRange.levelCount = 1;

    if (vkCreateImageView(device_.device, &yv, nullptr, &slot.yView) != VK_SUCCESS) {
        CUcontext popped = nullptr;
        cuCtxPopCurrent(&popped);
        return false;
    }
    // 创建uv缓冲区视图，声明如何操作缓冲区数据
    VkImageViewCreateInfo uvv{};
    uvv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    uvv.image = slot.uvImage;
    uvv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    uvv.format = VK_FORMAT_R8G8_UNORM;
    uvv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    uvv.subresourceRange.baseArrayLayer = 0;
    uvv.subresourceRange.layerCount = 1;
    uvv.subresourceRange.baseMipLevel = 0;
    uvv.subresourceRange.levelCount = 1;

    if (vkCreateImageView(device_.device, &uvv, nullptr, &slot.uvView) != VK_SUCCESS) {
        CUcontext popped = nullptr;
        cuCtxPopCurrent(&popped);
        return false;
    }

    // 得到导出数据缓冲区的句柄
    HANDLE yHandle = nullptr;
    HANDLE uvHandle = nullptr;
    if (!exportMemoryHandle(slot.yMemory, yHandle) || !yHandle ||
        !exportMemoryHandle(slot.uvMemory, uvHandle) || !uvHandle) {
        if (yHandle)
            CloseHandle(yHandle);
        if (uvHandle)
            CloseHandle(uvHandle);
        CUcontext popped = nullptr;
        cuCtxPopCurrent(&popped);
        return false;
    }

    // CUDA导入句柄，得到映射后的数据区域
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC yDesc{};
    yDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
    yDesc.handle.win32.handle = yHandle;
    yDesc.size = ySize;

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC uvDesc{};
    uvDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
    uvDesc.handle.win32.handle = uvHandle;
    uvDesc.size = uvSize;

    CUresult ry = cuImportExternalMemory(&slot.yExtMem, &yDesc);
    CUresult ruv = cuImportExternalMemory(&slot.uvExtMem, &uvDesc);

    // 关闭句柄
    CloseHandle(yHandle);
    CloseHandle(uvHandle);

    // 如果没有导入成功，则清理已声明成功的部分
    if (ry != CUDA_SUCCESS || ruv != CUDA_SUCCESS) {
        if (slot.yExtMem) {
            cuDestroyExternalMemory(slot.yExtMem);
            slot.yExtMem = nullptr;
        }
        if (slot.uvExtMem) {
            cuDestroyExternalMemory(slot.uvExtMem);
            slot.uvExtMem = nullptr;
        }
        CUcontext popped = nullptr;
        cuCtxPopCurrent(&popped);
        return false;
    }

    // 对外部区域进行映射，得到可操作地址
    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC yArrayDesc{};
    yArrayDesc.offset = 0;
    yArrayDesc.arrayDesc.Width = width;
    yArrayDesc.arrayDesc.Height = height;
    yArrayDesc.arrayDesc.Depth = 0;
    yArrayDesc.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
    yArrayDesc.arrayDesc.NumChannels = 1;
    yArrayDesc.numLevels = 1;

    if (cuExternalMemoryGetMappedMipmappedArray(&slot.yPtr, slot.yExtMem, &yArrayDesc) !=
        CUDA_SUCCESS) {
        CUcontext popped = nullptr;
        cuCtxPopCurrent(&popped);
        return false;
    }

    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC uvArrayDesc{};
    uvArrayDesc.offset = 0;
    uvArrayDesc.arrayDesc.Width = width / 2;
    uvArrayDesc.arrayDesc.Height = height / 2;
    uvArrayDesc.arrayDesc.Depth = 0;
    uvArrayDesc.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
    uvArrayDesc.arrayDesc.NumChannels = 2;
    uvArrayDesc.numLevels = 1;
    if (cuExternalMemoryGetMappedMipmappedArray(&slot.uvPtr, slot.uvExtMem, &uvArrayDesc) !=
        CUDA_SUCCESS) {
        CUcontext popped = nullptr;
        cuCtxPopCurrent(&popped);
        return false;
    }

    CUcontext popped = nullptr;
    cuCtxPopCurrent(&popped);

    // 记录帧宽、高、对齐大小
    slot.width = width;
    slot.height = height;
    slot.strideY = strideY;
    slot.strideUV = strideUV;
    return true;
}

void VulkanRenderer::ensureOsdTexture(const std::string &osdTextUtf8)
{
    // 如果两次的字符串一致，则不用重新生成
    if (osdTextUtf8 == lastOsdText_) {
        return;
    }

    // 更新当前字符串
    lastOsdText_ = osdTextUtf8;

    // 如果为空，则返回
    if (osdTextUtf8.empty()) {
        osdWidth_ = 0;
        osdHeight_ = 0;
        return;
    }

    // 得到OSD的位图，并验证是否有效
    const auto bmp = osdTextRenderer_.renderText(osdTextUtf8, 28, 6);
    if (bmp.width == 0 || bmp.height == 0 || bmp.rgba.empty()) {
        osdWidth_ = 0;
        osdHeight_ = 0;
        return;
    }

    // 按需决定是否需要重新生成OSD对应的vkImage
    if (osdImage_ == VK_NULL_HANDLE || bmp.width != osdWidth_ || bmp.height != osdHeight_) {
        // 等待设备完成当前任务
        if (device_.device) {
            vkDeviceWaitIdle(device_.device);
        }

        // 先清理之前的OSD图像相关
        if (osdImageView_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_.device, osdImageView_, nullptr);
            osdImageView_ = VK_NULL_HANDLE;
        }
        if (osdImage_ != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator_, osdImage_, osdImageAlloc_);
            osdImage_ = VK_NULL_HANDLE;
            osdImageAlloc_ = VK_NULL_HANDLE;
        }

        // 如果采样器为空，则生成采样器
        if (osdSampler_ == VK_NULL_HANDLE) {
            // 采样器创建信息
            // magFilter - 图像放大时的滤波方式
            // minFilter - 图像缩大时的滤波方式
            // addressModeU -
            // 指定采样坐标的u分量（对应通常的x分量）超出归一化坐标的[0,1)范围时的寻址模式
            // addressModeV -
            // 指定采样坐标的V分量（对应通常的y分量）超出归一化坐标的[0,1)范围时的寻址模式
            // addressModeW -
            // 指定采样坐标的s分量（对应通常的z分量）超出归一化坐标的[0,1)范围时的寻址模式
            // maxAnisotropy - 启用各向异性采样时，将各向异性数值钳制到不超过该值
            VkSamplerCreateInfo sci{};
            sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            sci.magFilter = VK_FILTER_LINEAR;
            sci.minFilter = VK_FILTER_LINEAR;
            sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.maxAnisotropy = 1.0f;
            if (vkCreateSampler(device_.device, &sci, nullptr, &osdSampler_) != VK_SUCCESS) {
                throw std::runtime_error("Failed to create OSD sampler");
            }
        }

        // 创建OSD图像
        // imageType - 图像类型
        // format - 图像的格式
        // extent - 图像的大小
        // mipLevels - 图像的mip等级数，应用于需要生成mipmap的图像
        // arrayLayers - 图像的图层数，用于图像数组
        // samples - 图像的采样方式
        // tiling - 图像数据的排列方式；最优排列
        // usage - 图像数据的用途；用于更新（外部写入）和采样
        // initialLayout - 图像初始布局；未定义
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {bmp.width, bmp.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;

        // 创建图像
        if (vmaCreateImage(allocator_, &ici, &aci, &osdImage_, &osdImageAlloc_, nullptr) !=
            VK_SUCCESS) {
            throw std::runtime_error("Failed to create OSD image");
        }

        // 创建图像视图，定义外部使用图像的接口
        // image - 视图绑定的图像
        // viewType - 图像类型
        // format - 图像格式
        // subresourceRange - view能看到(访问到)的image范围（子资源）
        //      aspectMask  - 图像的层面（如颜色、深度等等）
        //      levelCount  - 使用几级mip
        //      layerCount  - 使用几层layer
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = osdImage_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_.device, &vci, nullptr, &osdImageView_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create OSD image view");
        }

        // 更新osd区域的宽高以及初始布局
        // 设置最小宽度和最小高度
        osdWidth_ = bmp.width;
        osdHeight_ = bmp.height;
        osdImageLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

        // 更新OSD描述符集，加入采样器
        // 在更新前，确保 GPU 完全空闲，以避免描述符正在使用中被修改
        vkDeviceWaitIdle(device_.device);

        for (size_t i = 0; i < osdDescriptorSets_.size(); ++i) {
            // 描述符图像信息
            // sampler - 采样器
            // imageView - 图像视图
            // imageLayout - 图像所需的布局
            VkDescriptorImageInfo di{};
            di.sampler = osdSampler_;
            di.imageView = osdImageView_;
            di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            // 描述符更新信息
            // dstSet - 待更新的描述符集
            // dstBinding - 更新描述符中的哪个binding
            // descriptorCount - 更新的描述符数量
            // descriptorType - 描述符类型
            // pImageInfo - 带采样器的图像信息
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = osdDescriptorSets_[i];
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &di;
            vkUpdateDescriptorSets(device_.device, 1, &w, 0, nullptr);
        }
    }

    // 上传osd数据到对应的buffer
    ensureStagingBuffer(static_cast<VkDeviceSize>(bmp.width) * bmp.height * 4, osdStagingBuffer_,
                        osdStagingAlloc_, osdStagingMapped_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    // 通过buffer映射地址，进行更新
    std::memcpy(osdStagingMapped_, bmp.rgba.data(), bmp.rgba.size());
    vmaFlushAllocation(allocator_, osdStagingAlloc_, 0, VK_WHOLE_SIZE);
    osdUploadPending_ = true;
}

void VulkanRenderer::drawFrame(const decoder_sdk::Frame &frame, const std::string &osdTextUtf8)
{
    if (!initialized_) {
        return;
    }

    // 设置等待渲染的帧
    if (frame.isValid()) {
        pendingCudaFrame_ = frame;
        cudaFramePending_ = true;
    }

    // 当窗口尺寸发生变化是，重建交换链
    if (windowResized_) {
        recreateSwapchain();
    }

    // 确保已生成离屏渲染资源
    if (!ensureOffscreenResources()) {
        throw std::runtime_error("Failed to ensure offscreen resources!");
        return;
    }

    // 生成OSD纹理
    ensureOsdTexture(osdTextUtf8);

    // 等待当前帧变为空闲
    vkWaitForFences(device_.device, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);

    // 如果是调试模式，需要处理交换链相关
    // 找到交换链下一个将显示的图像索引
    uint32_t imageIndex = 0;
    if (debug_) {
        VkResult acquire = vkAcquireNextImageKHR(device_.device, swapchain_.swapchain, UINT64_MAX,
                                                 imageAvailableSemaphores_[currentFrame_],
                                                 VK_NULL_HANDLE, &imageIndex);

        // 当前交换链已过期时，重建交换链
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            return;
        }

        // 未能正确得到待显示的图像索引，报错
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            throw std::runtime_error("Failed to acquire swapchain image");
        }

        // 如果待显示的图像是飞行帧，则需要进行等待
        if (imageIndex < imagesInFlight_.size() && imagesInFlight_[imageIndex] != VK_NULL_HANDLE) {
            vkWaitForFences(device_.device, 1, &imagesInFlight_[imageIndex], VK_TRUE, UINT64_MAX);
        }

        // 记录待显示图像用的是哪个飞行帧的Fence，用来下一帧进行等待
        if (imageIndex < imagesInFlight_.size()) {
            imagesInFlight_[imageIndex] = inFlightFences_[currentFrame_];
        }
    }

    // Fence置位
    vkResetFences(device_.device, 1, &inFlightFences_[currentFrame_]);

    // cuda完成拷贝的同步信号
    VkSemaphore cudaWaitSemaphore = VK_NULL_HANDLE;
    // 是否在等待CUDA数据
    bool hasCudaWait = false;
    // Y图像
    VkImage cudaYImage = VK_NULL_HANDLE;
    // UV图像
    VkImage cudaUvImage = VK_NULL_HANDLE;
    if (cudaFramePending_ && pendingCudaFrame_.isValid() &&
        pendingCudaFrame_.pixelFormat() == decoder_sdk::ImageFormat::kCuda) {
        // 初始化CUDA互操作数据
        if (initCudaInterop()) {
            // 得到帧宽、高以及对齐大小
            const uint32_t w = static_cast<uint32_t>(pendingCudaFrame_.width());
            const uint32_t h = static_cast<uint32_t>(pendingCudaFrame_.height());
            const int strideY = pendingCudaFrame_.linesize(0);
            const int strideUV = pendingCudaFrame_.linesize(1);

            // 得到使用的互操作资源
            const size_t slotIndex =
                cudaSlots_.empty() ? 0u : (static_cast<size_t>(currentFrame_) % cudaSlots_.size());
            if (w > 0 && h > 0 && strideY > 0 && strideUV > 0 && slotIndex < cudaSlots_.size()) {
                // 互操作资源
                auto &slot = cudaSlots_[slotIndex];
                // 对应的资源描述符
                const uint32_t descriotorSetIndex = currentFrame_ % kMaxFramesInFlight;
                videoDescriptorsValid_[descriotorSetIndex] = false;

                // 保证CUDA互操作资源已申请
                if (ensureCudaInteropSlot(slot, w, h, strideY, strideUV)) {
                    if (cuCtxPushCurrent(cudaContext_) == CUDA_SUCCESS) {
                        const CUdeviceptr srcY =
                            reinterpret_cast<CUdeviceptr>(pendingCudaFrame_.data(0));
                        const CUdeviceptr srcUV =
                            reinterpret_cast<CUdeviceptr>(pendingCudaFrame_.data(1));

                        // 数据拷贝
                        if (srcY && srcUV) {
                            // 获得CUarray
                            CUarray yArray;
                            cuMipmappedArrayGetLevel(&yArray, slot.yPtr, 0);
                            // 拷贝
                            CUDA_MEMCPY2D cpyY{};
                            cpyY.srcMemoryType = CU_MEMORYTYPE_DEVICE;
                            cpyY.srcDevice = srcY;
                            cpyY.srcPitch = static_cast<size_t>(strideY);
                            cpyY.dstMemoryType = CU_MEMORYTYPE_ARRAY;
                            cpyY.dstArray = yArray;
                            cpyY.WidthInBytes = w;
                            cpyY.Height = h;
                            cuMemcpy2DAsync(&cpyY, cudaStream_);

                            // 获得CUarray
                            CUarray uvArray;
                            cuMipmappedArrayGetLevel(&uvArray, slot.uvPtr, 0);
                            // 拷贝
                            CUDA_MEMCPY2D cpyUV{};
                            cpyUV.srcMemoryType = CU_MEMORYTYPE_DEVICE;
                            cpyUV.srcDevice = srcUV;
                            cpyUV.srcPitch = static_cast<size_t>(strideUV);
                            cpyUV.dstMemoryType = CU_MEMORYTYPE_ARRAY;
                            cpyUV.dstArray = uvArray;
                            cpyUV.WidthInBytes = w;
                            cpyUV.Height = h >> 1;
                            cuMemcpy2DAsync(&cpyUV, cudaStream_);

                            // 使用信号量进行同步控制
                            CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS sig{};
                            CUexternalSemaphore sem = slot.cudaExtSemaphore;
                            cuSignalExternalSemaphoresAsync(&sem, &sig, 1, cudaStream_);

                            // 记录需要等待的数据
                            cudaWaitSemaphore = slot.cudaReadySemaphore;
                            hasCudaWait = true;
                            cudaYImage = slot.yImage;
                            cudaUvImage = slot.uvImage;

                            // 确定是否需要重建描述符资源
                            if (descriotorSetIndex < videoDescriptorSets_.size() &&
                                descriotorSetIndex < videoDescriptorsValid_.size() &&
                                !videoDescriptorsValid_[descriotorSetIndex]) {
                                // 在更新描述符前，确保 GPU 完全空闲，以避免描述符正在使用中被修改
                                vkDeviceWaitIdle(device_.device);

                                // 更新描述符资源，Y、UV分别更新
                                VkDescriptorImageInfo imageInfos[2]{};
                                imageInfos[0].sampler = videoSampler_;
                                imageInfos[0].imageView = slot.yView;
                                imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                                imageInfos[1].sampler = videoSampler_;
                                imageInfos[1].imageView = slot.uvView;
                                imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                                VkWriteDescriptorSet writes[2]{};
                                writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                                writes[0].dstSet = videoDescriptorSets_[descriotorSetIndex];
                                writes[0].dstBinding = 0;
                                writes[0].descriptorCount = 1;
                                writes[0].descriptorType =
                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                                writes[0].pImageInfo = &imageInfos[0];
                                writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                                writes[1].dstSet = videoDescriptorSets_[descriotorSetIndex];
                                writes[1].dstBinding = 1;
                                writes[1].descriptorCount = 1;
                                writes[1].descriptorType =
                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                                writes[1].pImageInfo = &imageInfos[1];
                                vkUpdateDescriptorSets(device_.device, 2, writes, 0, nullptr);
                                videoDescriptorsValid_[descriotorSetIndex] = true;
                            }

                            // 记录当前使用的视频描述符索引
                            if (descriotorSetIndex < videoDescriptorSets_.size()) {
                                lastVideoSetIndex_ = descriotorSetIndex;
                                hasVideoSet_ = true;
                            }
                        }

                        CUcontext popped = nullptr;
                        cuCtxPopCurrent(&popped);
                    }
                }
            }
        }

        // 视频帧已更新
        cudaFramePending_ = false;
    }

    // 重置命令缓冲区
    VkCommandBuffer cmd = commandBuffers_[currentFrame_];
    vkResetCommandBuffer(cmd, 0);

    // 命令缓冲区开始记录
    // flags - 标志位；只执行一次
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin command buffer");
    }

    // 如果OSD有更新
    if (osdUploadPending_ && osdImage_ != VK_NULL_HANDLE) {
        // OSD的布局是否已是被着色器读取
        const bool osdWasShaderRead = (osdImageLayout_ == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // 确定OSD当前的阶段
        const VkPipelineStageFlags osdSrcStage = osdWasShaderRead
                                                     ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                                     : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        // 确定OSD当前的使用方式
        const VkAccessFlags osdSrcAccess = osdWasShaderRead ? VK_ACCESS_SHADER_READ_BIT : 0;

        // 图像布局转换
        transitionImage(cmd, osdImage_, osdImageLayout_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        osdSrcStage, VK_PIPELINE_STAGE_TRANSFER_BIT, osdSrcAccess,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        // 数据拷贝
        copyBufferToImage(cmd, osdStagingBuffer_, osdImage_, osdWidth_, osdHeight_,
                          VK_IMAGE_ASPECT_COLOR_BIT);
        // 布局转换回着色器可读
        transitionImage(cmd, osdImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

        // 记录当前的OSD图像布局
        osdImageLayout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        osdUploadPending_ = false;
    }

    // 对Y、UV图像进行可见性同步
    // 注意：CUDA 写入的外部内存对应的是 VkImage，渲染侧也以 VkImage 进行采样。
    // 这里不做 layout 切换，仅建立从“外部写入”到“片元读取”的内存依赖。
    if (hasCudaWait && cudaYImage != VK_NULL_HANDLE && cudaUvImage != VK_NULL_HANDLE) {
        VkImageMemoryBarrier barriers[2]{};
        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].image = cudaYImage;
        barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].subresourceRange.baseMipLevel = 0;
        barriers[0].subresourceRange.levelCount = 1;
        barriers[0].subresourceRange.baseArrayLayer = 0;
        barriers[0].subresourceRange.layerCount = 1;

        barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[1].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].image = cudaUvImage;
        barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[1].subresourceRange.baseMipLevel = 0;
        barriers[1].subresourceRange.levelCount = 1;
        barriers[1].subresourceRange.baseArrayLayer = 0;
        barriers[1].subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2,
                             barriers);
    }

    uint32_t frameIndex = currentFrame_ % kMaxFramesInFlight;

    // ========== 第一阶段：渲染到离屏纹理 ==========
    // 确保离屏纹理的布局已转换为COLOR_ATTACHMENT_OPTIMAL
    // 只在必要时进行布局转换，避免重复转换
    if (vulkanSlots_[frameIndex].layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        vulkanSlots_[frameIndex].layout != VK_IMAGE_LAYOUT_UNDEFINED) {
        transitionImage(cmd, vulkanSlots_[frameIndex].image, vulkanSlots_[frameIndex].layout,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        vulkanSlots_[frameIndex].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    // 离屏渲染通道开始
    // renderPass - 离屏渲染通道
    // framebuffer - 离屏帧缓冲
    // offset - 渲染点偏移
    // extent - 渲染区域（使用离屏纹理尺寸）
    // clearValueCount - 清屏颜色值数量
    // pClearValues - 清屏颜色
    VkRenderPassBeginInfo offscreenRp{};
    offscreenRp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    offscreenRp.renderPass = offscreenRenderPass_;
    offscreenRp.framebuffer = vulkanSlots_[frameIndex].framebuffer;
    offscreenRp.renderArea.offset = {0, 0};
    offscreenRp.renderArea.extent = {offscreenWidth_, offscreenHeight_};
    VkClearValue offscreenClear{};
    offscreenClear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    offscreenRp.clearValueCount = 1;
    offscreenRp.pClearValues = &offscreenClear;

    vkCmdBeginRenderPass(cmd, &offscreenRp, VK_SUBPASS_CONTENTS_INLINE);

    // 设置渲染视口（离屏纹理尺寸）
    VkViewport offscreenViewport{};
    offscreenViewport.x = 0;
    offscreenViewport.y = 0;
    offscreenViewport.width = static_cast<float>(offscreenWidth_);
    offscreenViewport.height = static_cast<float>(offscreenHeight_);
    offscreenViewport.minDepth = 0.0f;
    offscreenViewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &offscreenViewport);

    // 设置裁剪区域（离屏纹理尺寸）
    VkRect2D offscreenScissor{};
    offscreenScissor.offset = {0, 0};
    offscreenScissor.extent = {offscreenWidth_, offscreenHeight_};
    vkCmdSetScissor(cmd, 0, 1, &offscreenScissor);

    // 顶点缓冲区偏移值
    VkDeviceSize vbOffset = 0;
    // 视频的资源描述符索引
    const uint32_t videoSetIndex =
        hasVideoSet_ ? lastVideoSetIndex_ : (currentFrame_ % kMaxFramesInFlight);
    // 视频资源是否准备好
    const bool videoReady =
        videoSetIndex < videoDescriptorsValid_.size() && videoDescriptorsValid_[videoSetIndex];

    if (videoReady) {
        // 绘制视频到离屏纹理
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, videoPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, videoPipelineLayout_, 0, 1,
                                &videoDescriptorSets_[videoSetIndex], 0, nullptr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &videoVertexBuffer_, &vbOffset);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }

    // 如果OSD数据有效
    if (osdWidth_ > 0 && osdHeight_ > 0 && osdImageView_ != VK_NULL_HANDLE) {
        // OSD 使用屏幕像素坐标构建一个矩形 quad，放置到右上角。
        // 顶点着色器通过 push constant 中的屏幕尺寸把像素坐标转换为 NDC。
        const float padding = 16.0f;
        float x0 = static_cast<float>(offscreenWidth_) - static_cast<float>(osdWidth_) - padding;
        if (x0 < padding) {
            x0 = padding;
        }
        const float y0 = padding;
        const float x1 = x0 + static_cast<float>(osdWidth_);
        const float y1 = y0 + static_cast<float>(osdHeight_);

        // 更新OSD点位
        const OsdVertex quad[6] = {
            {{x0, y0}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
            {{x1, y0}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
            {{x1, y1}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
            {{x0, y0}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
            {{x1, y1}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
            {{x0, y1}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        };
        std::memcpy(osdVertexMapped_, quad, sizeof(quad));
        vmaFlushAllocation(allocator_, osdVertexAlloc_, 0, VK_WHOLE_SIZE);

        // 绘制OSD到离屏纹理
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, osdPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, osdPipelineLayout_, 0, 1,
                                &osdDescriptorSets_[videoSetIndex], 0, nullptr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &osdVertexBuffer_, &vbOffset);
        const float screenSize[2] = {static_cast<float>(offscreenWidth_),
                                     static_cast<float>(offscreenHeight_)};
        vkCmdPushConstants(cmd, osdPipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(screenSize), screenSize);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }

    // 离屏渲染通道结束
    vkCmdEndRenderPass(cmd);

    // ========== 离屏纹理布局转换：color attachment -> shader read ==========
    // render pass 的 finalLayout 已自动转换布局到 SHADER_READ_ONLY_OPTIMAL
    // 此处仅更新状态跟踪，避免冗余转换
    vulkanSlots_[frameIndex].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    if (debug_) {
        // ========== 第二阶段：将离屏纹理渲染到交换链 ==========
        // 交换链渲染通道开始
        // renderPass - 交换链渲染通道
        // framebuffer - 交换链帧缓冲
        // offset - 渲染点偏移
        // extent - 渲染区域
        // clearValueCount - 清屏颜色值数量
        // pClearValues - 清屏颜色
        VkRenderPassBeginInfo swapchainRp{};
        swapchainRp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        swapchainRp.renderPass = renderPass_;
        swapchainRp.framebuffer = framebuffers_[imageIndex];
        swapchainRp.renderArea.offset = {0, 0};
        swapchainRp.renderArea.extent = swapchain_.extent;
        VkClearValue swapchainClear{};
        swapchainClear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        swapchainRp.clearValueCount = 1;
        swapchainRp.pClearValues = &swapchainClear;

        vkCmdBeginRenderPass(cmd, &swapchainRp, VK_SUBPASS_CONTENTS_INLINE);

        // 设置渲染视口（交换链尺寸）
        VkViewport swapchainViewport{};
        swapchainViewport.x = 0;
        swapchainViewport.y = 0;
        swapchainViewport.width = static_cast<float>(swapchain_.extent.width);
        swapchainViewport.height = static_cast<float>(swapchain_.extent.height);
        swapchainViewport.minDepth = 0.0f;
        swapchainViewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &swapchainViewport);

        // 设置裁剪区域（交换链尺寸）
        VkRect2D swapchainScissor{};
        swapchainScissor.offset = {0, 0};
        swapchainScissor.extent = swapchain_.extent;
        vkCmdSetScissor(cmd, 0, 1, &swapchainScissor);

        // 如果Blit管线已准备好，使用它来渲染离屏纹理到交换链
        if (blitPipeline_ != VK_NULL_HANDLE && !blitDescriptorSets_.empty()) {
            const uint32_t blitDescriptorIndex = currentFrame_ % kMaxFramesInFlight;
            if (blitDescriptorIndex < blitDescriptorSets_.size()) {
                // 绘制全屏四边形（采样离屏纹理）
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitPipeline_);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitPipelineLayout_,
                                        0, 1, &blitDescriptorSets_[blitDescriptorIndex], 0,
                                        nullptr);
                vkCmdBindVertexBuffers(cmd, 0, 1, &blitVertexBuffer_, &vbOffset);
                vkCmdDraw(cmd, 6, 1, 0, 0);
            }
        }

        // 交换链渲染通道结束
        vkCmdEndRenderPass(cmd);
    }

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        throw std::runtime_error("Failed to record command buffer");
    }

    VkSemaphore waitSems[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};

    uint32_t waitCount = 0;
    if (debug_) {
        appendSemaphore(imageAvailableSemaphores_[currentFrame_],
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, waitSems, waitStages,
                        static_cast<uint32_t>(std::size(waitSems)), waitCount);
    }
    if (hasCudaWait) {
        appendSemaphore(cudaWaitSemaphore, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, waitSems,
                        waitStages, static_cast<uint32_t>(std::size(waitSems)), waitCount);
    }

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = waitCount;
    submit.pWaitSemaphores = waitSems;
    submit.pWaitDstStageMask = waitStages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;

    VkSemaphore signalSems[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t signalCount = 0;
    if (debug_) {
        signalSems[signalCount++] = renderFinishedSemaphores_[imageIndex];
    }
    const bool shouldSignalCudaReady =
        frame.isValid() && frame.pixelFormat() == decoder_sdk::ImageFormat::kCuda &&
        frameIndex < vulkanSlots_.size() &&
        vulkanSlots_[frameIndex].cudaReadySemaphore != VK_NULL_HANDLE;
    if (shouldSignalCudaReady) {
        signalSems[signalCount++] = vulkanSlots_[frameIndex].cudaReadySemaphore;
        vulkanSlots_[frameIndex].hasPendingCudaSignal = true;
    } else if (frameIndex < vulkanSlots_.size()) {
        vulkanSlots_[frameIndex].hasPendingCudaSignal = false;
    }
    submit.signalSemaphoreCount = signalCount;
    submit.pSignalSemaphores = signalCount > 0 ? signalSems : nullptr;

    if (vkQueueSubmit(graphicsQueue_, 1, &submit, inFlightFences_[currentFrame_]) != VK_SUCCESS) {
        throw std::runtime_error("Failed to submit draw command buffer");
    }

    // 进行呈现，并设置等待此帧画面呈现完成的信号量
    if (debug_) {
        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderFinishedSemaphores_[imageIndex];
        VkSwapchainKHR sc = swapchain_.swapchain;
        present.swapchainCount = 1;
        present.pSwapchains = &sc;
        present.pImageIndices = &imageIndex;

        VkResult pres = vkQueuePresentKHR(presentQueue_, &present);
        if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
            recreateSwapchain();
        } else if (pres != VK_SUCCESS) {
            throw std::runtime_error("Failed to present swapchain image");
        }
    }

    // 更新飞行帧索引，开启下一次渲染
    lastFrame_ = currentFrame_;
    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}
