#include "Nv12Render_Vulkan.h"

#ifdef VULKAN_AVAILABLE
#include <QApplication>
#include <QByteArray>
#include <QFile>

#include <vulkan/vulkan.h>

#include <mutex>

namespace {
const char *vsrc = R"(
#ifdef GL_ES
    precision mediump float;
#endif
	
    attribute vec4 vertexIn;
    attribute vec2 textureIn;
    varying vec2 textureOut;
    void main(void)
    {
        gl_Position = vertexIn;
        textureOut = textureIn;
    }
)";

const char *fsrc = R"(
#ifdef GL_ES
    precision mediump float;
#endif

    uniform sampler2D texture;
    varying vec2 textureOut;

    void main(void)
    {
        gl_FragColor = texture2D(texture, textureOut);
    }
)";

typedef void(APIENTRYP PFNGLDELETEMEMORYOBJECTSEXTPROC)(GLsizei, const GLuint *);
typedef void(APIENTRYP PFNGLGENSEMAPHORESEXTPROC)(GLsizei n, GLuint *semaphores);
typedef void(APIENTRYP PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC)(GLuint semaphore, GLenum handleType,
                                                               HANDLE handle);
typedef void(APIENTRYP PFNGLWAITSEMAPHOREEXTPROC)(GLuint semaphore, GLuint numBufferBarriers,
                                                  const GLuint *buffers, GLuint numTextureBarriers,
                                                  const GLuint *textures, const GLenum *srcLayouts);
typedef void(APIENTRYP PFNGLCREATEMEMORYOBJECTSEXTPROC)(GLsizei n, GLuint *memoryObjects);
typedef void(APIENTRYP PFNGLDELETEMEMORYOBJECTSEXTPROC)(GLsizei n, const GLuint *memoryObjects);
typedef void(APIENTRYP PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC)(GLuint memory, GLuint64 size,
                                                            GLenum handleType, HANDLE handle);
typedef void(APIENTRYP PFNGLNAMEDBUFFERSTORAGEMEMEXTPROC)(GLuint buffer, GLuint64 size,
                                                          GLuint memory, GLuint64 offset);
typedef void(APIENTRYP PFNGLTEXTURESTORAGE2DPROC)(GLuint texture, GLsizei levels,
                                                  GLenum internalformat, GLsizei width,
                                                  GLsizei height);
typedef void(APIENTRYP PFNGLTEXTURESUBIMAGE2DPROC)(GLuint texture, GLint level, GLint xoffset,
                                                   GLint yoffset, GLsizei width, GLsizei height,
                                                   GLenum format, GLenum type, const void *pixels);
typedef void(APIENTRYP PFNGLTEXTUREPARAMETERIPROC)(GLuint texture, GLenum pname, GLint param);
typedef void(APIENTRYP PFNGLSIGNALSEMAPHOREEXTPROC)(GLuint semaphore, GLuint numBufferBarriers,
                                                    const GLuint *buffers,
                                                    GLuint numTextureBarriers,
                                                    const GLuint *textures,
                                                    const GLenum *srcLayouts);
typedef void(APIENTRYP PFNGLCREATETEXTURESPROC)(GLenum target, GLsizei n, GLuint *textures);
typedef void(APIENTRYP PFNGLDELETESEMAPHORESEXTPROC)(GLsizei n, const GLuint *semaphores);
typedef void(APIENTRYP PFNGLTEXTURESTORAGEMEM2DEXTPROC)(GLuint texture, GLsizei levels,
                                                        GLenum internalformat, GLsizei width,
                                                        GLsizei height, GLuint memory,
                                                        GLuint64 offset);

// 全局锁
static std::mutex g_mutex;

// gl函数扩展
static bool g_extLoaded = false;
static bool g_extLoadTried = false;

// 扩展函数指针
static PFNGLGENSEMAPHORESEXTPROC glGenSemaphoresEXT = nullptr;
static PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC glImportSemaphoreWin32HandleEXT = nullptr;
static PFNGLWAITSEMAPHOREEXTPROC glWaitSemaphoreEXT = nullptr;
static PFNGLCREATEMEMORYOBJECTSEXTPROC glCreateMemoryObjectsEXT = nullptr;
static PFNGLDELETEMEMORYOBJECTSEXTPROC glDeleteMemoryObjectsEXT = nullptr;
static PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC glImportMemoryWin32HandleEXT = nullptr;
static PFNGLSIGNALSEMAPHOREEXTPROC glSignalSemaphoreEXT = nullptr;
static PFNGLDELETESEMAPHORESEXTPROC glDeleteSemaphoresEXT = nullptr;
static PFNGLTEXTURESTORAGEMEM2DEXTPROC glTextureStorageMem2DEXT = nullptr;

static bool loadExtFunctions(QOpenGLContext *ctx)
{
    // 已经加载过就返回结果
    if (g_extLoadTried)
        return g_extLoaded;

    std::lock_guard l(g_mutex);
    // 获得锁之后，再确认一次
    if (g_extLoadTried)
        return g_extLoaded;

    g_extLoadTried = true;

    if (!ctx) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] LoadExtFunctions: no context!");
        g_extLoaded = false;
        return false;
    }

    auto loadFunc = [&](auto &func, const char *name) {
        if (!func) {
            void *addr = ctx->getProcAddress(name);

            // 部分驱动用 no-underscore，大部分是正确大小写
            if (!addr) {
                // 尝试不带 EXT 后缀
                const QString fallback = QString(name).replace("EXT", "");
                addr = ctx->getProcAddress(fallback.toLatin1().data());
            }

            // 存储结果
            func = reinterpret_cast<std::decay_t<decltype(func)>>(addr);

            if (!func) {
                qWarning()
                    << QStringLiteral("[Nv12Render_Vulkan] Missing extension func: %1").arg(name);
                return false;
            }
        }
        return true;
    };

    bool ok = true;

    ok &= loadFunc(glGenSemaphoresEXT, "glGenSemaphoresEXT");
    ok &= loadFunc(glImportSemaphoreWin32HandleEXT, "glImportSemaphoreWin32HandleEXT");
    ok &= loadFunc(glWaitSemaphoreEXT, "glWaitSemaphoreEXT");
    ok &= loadFunc(glCreateMemoryObjectsEXT, "glCreateMemoryObjectsEXT");
    ok &= loadFunc(glDeleteMemoryObjectsEXT, "glDeleteMemoryObjectsEXT");
    ok &= loadFunc(glImportMemoryWin32HandleEXT, "glImportMemoryWin32HandleEXT");
    ok &= loadFunc(glSignalSemaphoreEXT, "glSignalSemaphoreEXT");
    ok &= loadFunc(glDeleteSemaphoresEXT, "glDeleteSemaphoresEXT");
    ok &= loadFunc(glTextureStorageMem2DEXT, "glTextureStorageMem2DEXT");

    g_extLoaded = ok;
    return ok;
}

// vulkan着色器
static QByteArray g_vertShaderSrc;
static QByteArray g_fragShaderSrc;
static bool g_shaderLoaded = false;
static bool g_shaderLoadTried = false;

static bool loadShaderFunctions()
{
    // 已经加载过就返回结果
    if (g_shaderLoadTried)
        return g_shaderLoaded;

    std::lock_guard l(g_mutex);
    // 获得锁之后，再确认一次
    if (g_shaderLoadTried)
        return g_shaderLoaded;

    g_shaderLoadTried = true;

    const auto applicationDirPath = qApp->applicationDirPath();
    QFile vertFile(QStringLiteral("%1/shaders/ycbcr_to_rgba.vert.spv").arg(applicationDirPath));
    QFile fragFile(QStringLiteral("%1/shaders/ycbcr_to_rgba.frag.spv").arg(applicationDirPath));
    if (vertFile.open(QIODevice::ReadOnly)) {
        g_vertShaderSrc = vertFile.readAll();
        vertFile.close();
    }
    if (fragFile.open(QIODevice::ReadOnly)) {
        g_fragShaderSrc = fragFile.readAll();
        fragFile.close();
    }

    g_shaderLoaded = !g_vertShaderSrc.isEmpty() && !g_fragShaderSrc.isEmpty();
    return g_shaderLoaded;
}

static uint32_t getPlaneCount(VkFormat format)
{
    switch (format) {
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM: // YUV420P
        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM: // YUV422P
        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM: // YUV444P
            return 3;

        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: // NV12
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM: // NV16
            return 2;

        default:
            return 1; // 单平面格式
    }
}

static GLenum getPlaneFormatGL(VkFormat format, uint32_t planeIndex)
{
    switch (format) {
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
            // 所有平面都是8-bit单通道
            return GL_RG;

        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
            if (planeIndex == 0) {
                return GL_RG; // Y平面
            } else {
                return GL_RED; // UV平面（交错）
            }

        default:
            return GL_RGB; // 默认
    }
}

static VkImageAspectFlagBits getPlaneAspect(VkFormat format, uint32_t planeIndex)
{
    switch (format) {
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
            return static_cast<VkImageAspectFlagBits>(VK_IMAGE_ASPECT_PLANE_0_BIT << planeIndex);
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

static void getPlaneDimensions(VkFormat format, uint32_t width, uint32_t height,
                               uint32_t planeIndex, uint32_t &outWidth, uint32_t &outHeight)
{
    switch (format) {
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM: // YUV420P
            if (planeIndex == 0) {
                outWidth = width;
                outHeight = height;
            } else {
                outWidth = (width + 1) / 2;   // UV平面宽度减半
                outHeight = (height + 1) / 2; // UV平面高度减半
            }
            break;

        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: // NV12
            if (planeIndex == 0) {
                outWidth = width;
                outHeight = height;
            } else {
                outWidth = (width + 1) / 2;   // UV平面宽度减半
                outHeight = (height + 1) / 2; // UV平面高度减半
            }
            break;

        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM: // YUV422P
            if (planeIndex == 0) {
                outWidth = width;
                outHeight = height;
            } else {
                outWidth = (width + 1) / 2; // UV平面宽度减半
                outHeight = height;         // 高度不变
            }
            break;

        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM: // NV16
            if (planeIndex == 0) {
                outWidth = width;
                outHeight = height;
            } else {
                outWidth = (width + 1) / 2; // UV平面宽度减半
                outHeight = height;         // 高度不变
            }
            break;

        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM: // YUV444P
            outWidth = width;
            outHeight = height;
            break;

        default:
            outWidth = width;
            outHeight = height;
    }
}

/* Converts return values to strings */
const char *vkRet2str(VkResult res)
{
#define CASE(VAL) \
    case VAL:     \
        return #VAL
    switch (res) {
        CASE(VK_SUCCESS);
        CASE(VK_NOT_READY);
        CASE(VK_TIMEOUT);
        CASE(VK_EVENT_SET);
        CASE(VK_EVENT_RESET);
        CASE(VK_INCOMPLETE);
        CASE(VK_ERROR_OUT_OF_HOST_MEMORY);
        CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY);
        CASE(VK_ERROR_INITIALIZATION_FAILED);
        CASE(VK_ERROR_DEVICE_LOST);
        CASE(VK_ERROR_MEMORY_MAP_FAILED);
        CASE(VK_ERROR_LAYER_NOT_PRESENT);
        CASE(VK_ERROR_EXTENSION_NOT_PRESENT);
        CASE(VK_ERROR_FEATURE_NOT_PRESENT);
        CASE(VK_ERROR_INCOMPATIBLE_DRIVER);
        CASE(VK_ERROR_TOO_MANY_OBJECTS);
        CASE(VK_ERROR_FORMAT_NOT_SUPPORTED);
        CASE(VK_ERROR_FRAGMENTED_POOL);
        CASE(VK_ERROR_UNKNOWN);
        CASE(VK_ERROR_OUT_OF_POOL_MEMORY);
        CASE(VK_ERROR_INVALID_EXTERNAL_HANDLE);
        CASE(VK_ERROR_FRAGMENTATION);
        CASE(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS);
        CASE(VK_PIPELINE_COMPILE_REQUIRED);
        CASE(VK_ERROR_SURFACE_LOST_KHR);
        CASE(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR);
        CASE(VK_SUBOPTIMAL_KHR);
        CASE(VK_ERROR_OUT_OF_DATE_KHR);
        CASE(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR);
        CASE(VK_ERROR_VALIDATION_FAILED_EXT);
        CASE(VK_ERROR_INVALID_SHADER_NV);
        CASE(VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR);
        CASE(VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR);
        CASE(VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR);
        CASE(VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR);
        CASE(VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR);
        CASE(VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT);
        CASE(VK_ERROR_NOT_PERMITTED_KHR);
        CASE(VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT);
        CASE(VK_THREAD_IDLE_KHR);
        CASE(VK_THREAD_DONE_KHR);
        CASE(VK_OPERATION_DEFERRED_KHR);
        CASE(VK_OPERATION_NOT_DEFERRED_KHR);
        default:
            return "Unknown error";
    }
#undef CASE
}
} // namespace

Nv12Render_Vulkan::Nv12Render_Vulkan()
    : VideoRender(),
      vkInstance_{vulkan_utils::getVkInstance()},
      vkPhysicalDevice_{vulkan_utils::getVkPhysicalDevice()},
      vkDevice_{vulkan_utils::getVkDevice()},
      vkInstanceDispatchTable_{vulkan_utils::getInstanceDispatchTable()},
      vkDispatchTable_{vulkan_utils::getDispatchTable()}
{
}

Nv12Render_Vulkan::~Nv12Render_Vulkan()
{
    cleanupVulkanResources();
    cleanupOpenGLResources();

    vbo_.destroy();
}

bool Nv12Render_Vulkan::initRenderVbo(const bool horizontal, const bool vertical)
{
    initDefaultVBO(vbo_, horizontal, vertical);
    return true;
}

bool Nv12Render_Vulkan::initRenderShader(const decoder_sdk::Frame &frame)
{
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vsrc);
    program_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fsrc);
    program_.link();

    return true;
}

bool Nv12Render_Vulkan::initRenderTexture(const decoder_sdk::Frame &frame)
{
    return true;
}

bool Nv12Render_Vulkan::initInteropsResource(const decoder_sdk::Frame &frame)
{
    if (!loadExtFunctions(QOpenGLContext::currentContext()) || !loadShaderFunctions() ||
        !vulkan_utils::isVulkanAvaliable()) {
        return false;
    }

    const bool result = initInteropResources(frame.width(), frame.height());
    return result;
}

bool Nv12Render_Vulkan::renderFrame(const decoder_sdk::Frame &frame)
{
    // Vulkan离屏渲染将NV12转为RGBA
    if (!convertNV12ToRGBA(frame)) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to convert NV12 to RGBA.");
        return false;
    }

    // OpenGL渲染共享的RGBA纹理
    drawFrame(glRGBATexture_);

    return true;
}

bool Nv12Render_Vulkan::initGraphicsPipeline(uint32_t width, uint32_t height)
{
    const auto queueIndex = vkDevice_.get_queue_index(vkb::QueueType::graphics);
    if (!queueIndex.has_value()) {
        return false;
    }
    graphicsQueueIndex_ = queueIndex.value();
    vkDispatchTable_.fp_vkGetDeviceQueue(vkDevice_.device, graphicsQueueIndex_, 0, &graphicsQueue_);

    VkResult ret = VK_SUCCESS;

    VkCommandPoolCreateInfo cmdPoolInfo{};
    cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmdPoolInfo.queueFamilyIndex = graphicsQueueIndex_;
    cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    ret = vkDispatchTable_.fp_vkCreateCommandPool(vkDevice_.device, &cmdPoolInfo, nullptr,
                                                  &commandPool_);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to create command pool: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }
    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = commandPool_;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    ret = vkDispatchTable_.fp_vkAllocateCommandBuffers(vkDevice_.device, &cmdAllocInfo,
                                                       &commandBuffer_);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to create command buffers: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    // yCbCr
    VkSamplerYcbcrConversionCreateInfo convCreate{};
    convCreate.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
    convCreate.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    convCreate.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    convCreate.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    convCreate.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                             VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    convCreate.xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
    convCreate.yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;

    // 查询是否支持线性滤波
    VkFormatProperties2 formatProps2 = {};
    formatProps2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;

    VkFormatProperties3 formatProps3 = {};
    formatProps3.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;
    formatProps2.pNext = &formatProps3; // 链接到主结构

    vkInstanceDispatchTable_.fp_vkGetPhysicalDeviceFormatProperties2(
        vkPhysicalDevice_.physical_device, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &formatProps2);

    // 检查色度重采样所需的线性滤波支持
    const bool chromaLinearSupported =
        (formatProps3.optimalTilingFeatures &
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_CHROMA_RECONSTRUCTION_EXPLICIT_BIT) != 0;

    convCreate.chromaFilter = chromaLinearSupported ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    convCreate.forceExplicitReconstruction = VK_FALSE;

    ret = vkDispatchTable_.fp_vkCreateSamplerYcbcrConversion(vkDevice_.device, &convCreate, nullptr,
                                                             &ycbcrConversion_);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral(
                          "[Nv12Render_Vulkan] Failed to create YCbCr conversion sampler: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    // Sampler
    VkSamplerYcbcrConversionInfo convInfo{};
    convInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    convInfo.conversion = ycbcrConversion_;

    // 检查滤波器采样所需的线性滤波支持
    const bool sampleLinearSupported = (formatProps3.optimalTilingFeatures &
                                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;

    VkSamplerCreateInfo sampInfo{};
    sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampInfo.pNext = &convInfo;
    sampInfo.magFilter = sampleLinearSupported ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sampInfo.minFilter = sampleLinearSupported ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sampInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.maxAnisotropy = 1.0f;
    sampInfo.minLod = 0.0f;
    sampInfo.maxLod = 0.0f;

    ret = vkDispatchTable_.fp_vkCreateSampler(vkDevice_.device, &sampInfo, nullptr, &ycbcrSampler_);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to create sampler: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 2;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 2;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    ret = vkDispatchTable_.fp_vkCreateDescriptorPool(vkDevice_.device, &poolInfo, nullptr,
                                                     &descriptorPool_);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to create descriptor pool: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers = &ycbcrSampler_;
    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 1;
    dslInfo.pBindings = &binding;

    ret = vkDispatchTable_.fp_vkCreateDescriptorSetLayout(vkDevice_.device, &dslInfo, nullptr,
                                                          &descriptorSetLayout_);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to create descriptor layout: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &descriptorSetLayout_;

    ret = vkDispatchTable_.fp_vkCreatePipelineLayout(vkDevice_.device, &plInfo, nullptr,
                                                     &graphicsPipelineLayout_);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to create pipeline layout: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    VkAttachmentDescription colorAttach{};
    colorAttach.format = VK_FORMAT_R8G8B8A8_UNORM;
    colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttach.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttach.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &colorAttach;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dep;

    ret = vkDispatchTable_.fp_vkCreateRenderPass(vkDevice_.device, &rpInfo, nullptr, &renderPass_);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to create render pass: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    // 创建着色器
    if (g_vertShaderSrc.isEmpty() || g_fragShaderSrc.isEmpty()) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to read shaders.");
        return false;
    }

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = static_cast<size_t>(g_vertShaderSrc.size());
    smInfo.pCode = reinterpret_cast<const uint32_t *>(g_vertShaderSrc.constData());
    VkShaderModule vertModule = VK_NULL_HANDLE;

    ret = vkDispatchTable_.fp_vkCreateShaderModule(vkDevice_.device, &smInfo, nullptr, &vertModule);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to create vert shader module: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    smInfo.codeSize = static_cast<size_t>(g_fragShaderSrc.size());
    smInfo.pCode = reinterpret_cast<const uint32_t *>(g_fragShaderSrc.constData());
    VkShaderModule fragModule = VK_NULL_HANDLE;
    ret = vkDispatchTable_.fp_vkCreateShaderModule(vkDevice_.device, &smInfo, nullptr, &fragModule);
    if (ret != VK_SUCCESS) {
        vkDispatchTable_.fp_vkDestroyShaderModule(vkDevice_.device, vertModule, nullptr);
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to create frag shader module: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpState{};
    vpState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpState.viewportCount = 1;
    vpState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cbAtt{};
    cbAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cbAtt.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cbAtt;

    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vpState;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = graphicsPipelineLayout_;
    gp.renderPass = renderPass_;
    gp.subpass = 0;

    ret = vkDispatchTable_.fp_vkCreateGraphicsPipelines(vkDevice_.device, VK_NULL_HANDLE, 1, &gp,
                                                        nullptr, &graphicsPipeline_);
    if (ret != VK_SUCCESS) {
        vkDispatchTable_.fp_vkDestroyShaderModule(vkDevice_.device, vertModule, nullptr);
        vkDispatchTable_.fp_vkDestroyShaderModule(vkDevice_.device, fragModule, nullptr);
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to create graphics pipeline: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }
    vkDispatchTable_.fp_vkDestroyShaderModule(vkDevice_.device, vertModule, nullptr);
    vkDispatchTable_.fp_vkDestroyShaderModule(vkDevice_.device, fragModule, nullptr);

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = renderPass_;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &rgbaImageView_;
    fbInfo.width = width;
    fbInfo.height = height;
    fbInfo.layers = 1;

    ret =
        vkDispatchTable_.fp_vkCreateFramebuffer(vkDevice_.device, &fbInfo, nullptr, &framebuffer_);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to create frame buffer: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    return true;
}

bool Nv12Render_Vulkan::initInteropResources(uint32_t width, uint32_t height)
{
    if (isInteropInitialized_) {
        return true;
    }

    // 先清理可能存在的资源
    cleanupVulkanResources();
    cleanupOpenGLResources();

    VkResult ret = VK_SUCCESS;

    // 创建RGBA输出图像（用于渲染管线离屏渲染）
    VkExternalMemoryImageCreateInfo externalImageInfo = {};
    externalImageInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalImageInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkImageCreateInfo imageCreateInfo = {};
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.pNext = &externalImageInfo;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageCreateInfo.extent = {width, height, 1};
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    ret =
        vkDispatchTable_.fp_vkCreateImage(vkDevice_.device, &imageCreateInfo, nullptr, &rgbaImage_);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to create RGBA output image: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    // 分配并导出内存
    VkMemoryRequirements memReqs;
    vkDispatchTable_.fp_vkGetImageMemoryRequirements(vkDevice_.device, rgbaImage_, &memReqs);

    VkExportMemoryAllocateInfo exportMemoryInfo = {};
    exportMemoryInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportMemoryInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    // 根据实际设备，确定是否分配专用内存
    VkMemoryDedicatedAllocateInfo dedicatedAlloc{};
    void *allocPNext = &exportMemoryInfo;

    VkMemoryDedicatedRequirements dedReq{};
    dedReq.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;

    VkMemoryRequirements2 req2{};
    req2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    req2.pNext = &dedReq;

    VkImageMemoryRequirementsInfo2 imgReqInfo{};
    imgReqInfo.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    imgReqInfo.image = rgbaImage_;

    vkDispatchTable_.fp_vkGetImageMemoryRequirements2(vkDevice_.device, &imgReqInfo, &req2);
    memReqs = req2.memoryRequirements;

    const VkBool32 useDed = dedReq.prefersDedicatedAllocation | dedReq.requiresDedicatedAllocation;
    if (useDed) {
        dedicatedAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicatedAlloc.image = rgbaImage_;
        dedicatedAlloc.pNext = allocPNext;
        allocPNext = &dedicatedAlloc;
    }

    VkMemoryAllocateInfo memAllocInfo = {};
    memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAllocInfo.pNext = allocPNext;
    memAllocInfo.allocationSize = memReqs.size;
    // 需要找到支持导出的内存类型
    memAllocInfo.memoryTypeIndex =
        findMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    ret = vkDispatchTable_.fp_vkAllocateMemory(vkDevice_.device, &memAllocInfo, nullptr,
                                               &rgbaMemory_);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to allocate RGBA memory: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    ret = vkDispatchTable_.fp_vkBindImageMemory(vkDevice_.device, rgbaImage_, rgbaMemory_, 0);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to bind RGBA image memory: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    // 创建图像视图
    VkImageViewCreateInfo viewCreateInfo = {};
    viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCreateInfo.image = rgbaImage_;
    viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCreateInfo.subresourceRange.baseMipLevel = 0;
    viewCreateInfo.subresourceRange.levelCount = 1;
    viewCreateInfo.subresourceRange.baseArrayLayer = 0;
    viewCreateInfo.subresourceRange.layerCount = 1;

    ret = vkDispatchTable_.fp_vkCreateImageView(vkDevice_.device, &viewCreateInfo, nullptr,
                                                &rgbaImageView_);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to create RGBA image view: %1")
                          .arg(vkRet2str(ret));
        return false;
    }

    // 导出Vulkan内存到OpenGL
    HANDLE memoryHandle = nullptr;
    if (!exportMemoryHandle(rgbaMemory_, memoryHandle)) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to export Vulkan memory handle.");
        return false;
    }

    // 在OpenGL中导入内存和创建纹理
    glCreateMemoryObjectsEXT(1, &glMemoryObject_);
    glImportMemoryWin32HandleEXT(glMemoryObject_, memReqs.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                 memoryHandle);

    glGenTextures(1, &glRGBATexture_);
    glBindTexture(GL_TEXTURE_2D, glRGBATexture_);
    glTextureStorageMem2DEXT(glRGBATexture_, 1, GL_RGBA8, width, height, glMemoryObject_, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // 初始化渲染管线
    if (!initGraphicsPipeline(width, height)) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to initialize graphics pipeline.");
        return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = 0;
    ret = vkDispatchTable_.fp_vkCreateFence(vkDevice_.device, &fenceInfo, nullptr, &fence_);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to create vulkan fence: %1")
                          .arg(vkRet2str(ret));
        return false;
    }
    vkDispatchTable_.fp_vkResetFences(vkDevice_.device, 1, &fence_);

    isInteropInitialized_ = true;
    return true;
}

bool Nv12Render_Vulkan::convertNV12ToRGBA(const decoder_sdk::Frame &frame)
{
    if (!isInteropInitialized_ || graphicsPipeline_ == VK_NULL_HANDLE ||
        renderPass_ == VK_NULL_HANDLE || framebuffer_ == VK_NULL_HANDLE ||
        ycbcrSampler_ == VK_NULL_HANDLE) {
        qWarning() << QStringLiteral(
            "[Nv12Render_Vulkan] Interop or graphics pipeline not initialized.");
        return false;
    }

    struct Guard {
        const decoder_sdk::Frame &f;
        std::shared_ptr<decoder_sdk::VulkanFrame> vkFrame;
        Guard(const decoder_sdk::Frame &f) : f(f)
        {
            vkFrame = f.lockVulkanFrame();
        }
        ~Guard()
        {
            f.unlockVulkanFrame(vkFrame);
        }
    } guard(frame);

    const auto &vkFrame = guard.vkFrame;
    if (!vkFrame || vkFrame->format[0] != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
        qWarning() << QStringLiteral(
            "[Nv12Render_Vulkan] VkFrame is invalid or vkFrame foramt is invalid.");
        return false;
    }

    for (int i = 0; i < 8 && vkFrame->sem[i] != VK_NULL_HANDLE; ++i) {
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &vkFrame->sem[i];
        waitInfo.pValues = &vkFrame->sem_value[i];
        vkDispatchTable_.fp_vkWaitSemaphores(vkDevice_.device, &waitInfo, UINT64_MAX);
    }

    const auto frameWidth = static_cast<uint32_t>(frame.width());
    const auto frameHeight = static_cast<uint32_t>(frame.height());
    const VkFormat nv12Format = vkFrame->format[0];
    VkImage nv12Image = vkFrame->img[0];
    VkResult ret = VK_SUCCESS;

    VkImageViewCreateInfo nv12ViewInfo{};
    nv12ViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    nv12ViewInfo.image = nv12Image;
    nv12ViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    nv12ViewInfo.format = nv12Format;
    nv12ViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    nv12ViewInfo.subresourceRange.baseMipLevel = 0;
    nv12ViewInfo.subresourceRange.levelCount = 1;
    nv12ViewInfo.subresourceRange.baseArrayLayer = 0;
    nv12ViewInfo.subresourceRange.layerCount = 1;

    // 关联 YCbCr 转换
    VkSamplerYcbcrConversionInfo conversionInfo = {};
    conversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    conversionInfo.conversion = ycbcrConversion_;
    nv12ViewInfo.pNext = &conversionInfo;

    VkImageView nv12View = VK_NULL_HANDLE;
    ret =
        vkDispatchTable_.fp_vkCreateImageView(vkDevice_.device, &nv12ViewInfo, nullptr, &nv12View);
    if (ret != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to cteate NV12 image view: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    if (descriptorSet_ == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &descriptorSetLayout_;

        ret = vkDispatchTable_.fp_vkAllocateDescriptorSets(vkDevice_.device, &allocInfo,
                                                           &descriptorSet_);
        if (ret != VK_SUCCESS) {
            vkDispatchTable_.fp_vkDestroyImageView(vkDevice_.device, nv12View, nullptr);
            qWarning() << QStringLiteral(
                              "[Nv12Render_Vulkan] Failed to allocate descriptor sets: %1.")
                              .arg(vkRet2str(ret));
            return false;
        }
    }

    VkDescriptorImageInfo nv12Combined{};
    nv12Combined.sampler = ycbcrSampler_;
    nv12Combined.imageView = nv12View;
    nv12Combined.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &nv12Combined;
    vkDispatchTable_.fp_vkUpdateDescriptorSets(vkDevice_.device, 1, &write, 0, nullptr);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ret = vkDispatchTable_.fp_vkBeginCommandBuffer(commandBuffer_, &beginInfo);
    if (ret != VK_SUCCESS) {
        vkDispatchTable_.fp_vkDestroyImageView(vkDevice_.device, nv12View, nullptr);
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to begin command buffer: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    VkImageMemoryBarrier preBarriers[2]{};
    preBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[0].srcAccessMask = 0;
    preBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    preBarriers[0].oldLayout = vkFrame->layout[0];
    preBarriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    preBarriers[0].image = nv12Image;
    preBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    preBarriers[0].subresourceRange.baseMipLevel = 0;
    preBarriers[0].subresourceRange.levelCount = 1;
    preBarriers[0].subresourceRange.baseArrayLayer = 0;
    preBarriers[0].subresourceRange.layerCount = 1;

    preBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[1].srcAccessMask = 0;
    preBarriers[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    preBarriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    preBarriers[1].image = rgbaImage_;
    preBarriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    preBarriers[1].subresourceRange.baseMipLevel = 0;
    preBarriers[1].subresourceRange.levelCount = 1;
    preBarriers[1].subresourceRange.baseArrayLayer = 0;
    preBarriers[1].subresourceRange.layerCount = 1;

    vkDispatchTable_.fp_vkCmdPipelineBarrier(
        commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
        nullptr, 0, nullptr, 2, preBarriers);

    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderPass_;
    rpBegin.framebuffer = framebuffer_;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = {frameWidth, frameHeight};
    rpBegin.clearValueCount = 1;
    rpBegin.pClearValues = &clear;
    vkDispatchTable_.fp_vkCmdBeginRenderPass(commandBuffer_, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    vkDispatchTable_.fp_vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          graphicsPipeline_);
    vkDispatchTable_.fp_vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                graphicsPipelineLayout_, 0, 1, &descriptorSet_, 0,
                                                nullptr);

    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = static_cast<float>(frameWidth);
    vp.height = static_cast<float>(frameHeight);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.offset = {0, 0};
    sc.extent = {frameWidth, frameHeight};
    vkDispatchTable_.fp_vkCmdSetViewport(commandBuffer_, 0, 1, &vp);
    vkDispatchTable_.fp_vkCmdSetScissor(commandBuffer_, 0, 1, &sc);
    vkDispatchTable_.fp_vkCmdDraw(commandBuffer_, 3, 1, 0, 0);

    vkDispatchTable_.fp_vkCmdEndRenderPass(commandBuffer_);

    VkImageMemoryBarrier postBarrier[2]{};
    postBarrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    postBarrier[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    postBarrier[0].dstAccessMask = 0;
    postBarrier[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    postBarrier[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    postBarrier[0].image = rgbaImage_;
    postBarrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    postBarrier[0].subresourceRange.baseMipLevel = 0;
    postBarrier[0].subresourceRange.levelCount = 1;
    postBarrier[0].subresourceRange.baseArrayLayer = 0;
    postBarrier[0].subresourceRange.layerCount = 1;

    postBarrier[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    postBarrier[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    postBarrier[1].dstAccessMask = 0;
    postBarrier[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    postBarrier[1].newLayout = vkFrame->layout[0];
    postBarrier[1].image = nv12Image;
    postBarrier[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    postBarrier[1].subresourceRange.baseMipLevel = 0;
    postBarrier[1].subresourceRange.levelCount = 1;
    postBarrier[1].subresourceRange.baseArrayLayer = 0;
    postBarrier[1].subresourceRange.layerCount = 1;
    vkDispatchTable_.fp_vkCmdPipelineBarrier(
        commandBuffer_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, postBarrier);

    ret = vkDispatchTable_.fp_vkEndCommandBuffer(commandBuffer_);
    if (ret != VK_SUCCESS) {
        vkDispatchTable_.fp_vkDestroyImageView(vkDevice_.device, nv12View, nullptr);
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to end command buffer: %1.")
                          .arg(vkRet2str(ret));
        return false;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer_;

    {
        struct QueueGuard {
            const decoder_sdk::Frame &f;
            uint32_t queueFamily;
            uint32_t queueIndex;

            QueueGuard(const decoder_sdk::Frame &f, uint32_t queueFamily, uint32_t queueIndex)
                : f(f), queueFamily(queueFamily), queueIndex(queueIndex)

            {
                f.lockVulkanQueue(queueFamily, queueIndex);
            }
            ~QueueGuard()
            {
                f.unlockVulkanQueue(queueFamily, queueIndex);
            }
        } guard(frame, graphicsQueueIndex_, 0);
        ret = vkDispatchTable_.fp_vkQueueSubmit(graphicsQueue_, 1, &submitInfo, fence_);
        if (ret != VK_SUCCESS) {
            vkDispatchTable_.fp_vkDestroyImageView(vkDevice_.device, nv12View, nullptr);
            qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to submit queue: %1.")
                              .arg(vkRet2str(ret));
            return false;
        }
    }

    vkDispatchTable_.fp_vkWaitForFences(vkDevice_.device, 1, &fence_, VK_TRUE, UINT64_MAX);
    vkDispatchTable_.fp_vkResetFences(vkDevice_.device, 1, &fence_);

    vkDispatchTable_.fp_vkDestroyImageView(vkDevice_.device, nv12View, nullptr);
    return true;
}

void Nv12Render_Vulkan::drawFrame(GLuint rgbaTexture)
{
    if (rgbaTexture == 0) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Invalid RGBA texture.");
        return;
    }

    program_.bind();
    vbo_.bind();
    program_.enableAttributeArray("vertexIn");
    program_.enableAttributeArray("textureIn");
    program_.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 2, 0);
    program_.setAttributeBuffer("textureIn", GL_FLOAT, 2 * 4 * sizeof(GLfloat), 2, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, rgbaTexture);
    program_.setUniformValue("texture", 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    program_.disableAttributeArray("vertexIn");
    program_.disableAttributeArray("textureIn");
    vbo_.release();
    program_.release();
}

#ifdef _WIN32
bool Nv12Render_Vulkan::exportMemoryHandle(VkDeviceMemory memory, HANDLE &outHandle)
{
    // 获取Win32内存句柄函数
    auto vkGetMemoryWin32HandleKHR = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
        vkDevice_.fp_vkGetDeviceProcAddr(vkDevice_.device, "vkGetMemoryWin32HandleKHR"));
    if (!vkGetMemoryWin32HandleKHR) {
        qWarning() << QStringLiteral(
            "[Nv12Render_Vulkan] Not supported vkGetMemoryWin32HandleKHR.");
        return false;
    }

    VkMemoryGetWin32HandleInfoKHR getHandleInfo = {};
    getHandleInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    getHandleInfo.memory = memory;
    getHandleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    const VkResult result = vkGetMemoryWin32HandleKHR(vkDevice_.device, &getHandleInfo, &outHandle);
    if (result != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to get Win32 memory handle: %1.")
                          .arg(vkRet2str(result));
        return false;
    }

    return true;
}
#else
bool Nv12Render_Vulkan::exportMemoryHandle(VkDeviceMemory memory, int &outFd)
{
    // 获取linux内存句柄函数
    auto vkGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkDevice_.fp_vkGetDeviceProcAddr(vkDevice_.device, "vkGetMemoryFdKHR"));
    if (!vkGetMemoryFdKHR) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Not supported vkGetMemoryFdKHR.");
        return false;
    }

    VkMemoryGetFdInfoKHR getFdInfo = {};
    getFdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    getFdInfo.memory = memory;
    getFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    const VkResult result = vkGetMemoryFdKHR(vkDevice_.device, &getFdInfo, &outFd);
    if (result != VK_SUCCESS) {
        qWarning() << QStringLiteral(
                          "[Nv12Render_Vulkan] Failed to get memory file descriptor: %1.")
                          .arg(vkRet2str(result));
        return false;
    }

    return true;
}
#endif

#ifdef _WIN32
bool Nv12Render_Vulkan::exportSemaphoreHandle(VkSemaphore semaphore, HANDLE &outHandle)
{
    // 获取Win32信号量句柄函数
    auto vkGetSemaphoreWin32HandleKHR = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
        vkDevice_.fp_vkGetDeviceProcAddr(vkDevice_.device, "vkGetSemaphoreWin32HandleKHR"));
    if (!vkGetSemaphoreWin32HandleKHR) {
        qWarning() << QStringLiteral(
            "[Nv12Render_Vulkan] Not supported vkGetSemaphoreWin32HandleKHR.");
        return false;
    }

    VkSemaphoreGetWin32HandleInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
    info.semaphore = semaphore;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    const VkResult result = vkGetSemaphoreWin32HandleKHR(vkDevice_.device, &info, &outHandle);
    if (result != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to export semaphore handle: %1.")
                          .arg(vkRet2str(result));
        return false;
    }

    return true;
}
#else
bool Nv12Render_Vulkan::exportSemaphoreHandle(VkSemaphore semaphore, int &outFd)
{
    // 获取Linux信号量句柄函数
    auto vkGetSemaphoreFdKHR = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        vkDevice_.fp_vkGetDeviceProcAddr(vkDevice_.device, "vkGetSemaphoreFdKHR"));
    if (!vkGetSemaphoreFdKHR) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Not supported vkGetSemaphoreFdKHR.");
        return false;
    }

    VkSemaphoreGetFdInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    info.semaphore = semaphore;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    const VkResult result = vkGetSemaphoreFdKHR(vkDevice_.device, &info, &outFd);
    if (result != VK_SUCCESS) {
        qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to export semaphore handle: %1.")
                          .arg(vkRet2str(result));
        return false;
    }

    return true;
}
#endif

uint32_t Nv12Render_Vulkan::findMemoryTypeIndex(uint32_t typeFilter,
                                                VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProperties = {};
    vkInstanceDispatchTable_.fp_vkGetPhysicalDeviceMemoryProperties(
        vkPhysicalDevice_.physical_device, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        // typeFilter 是一个掩码，判断当前内存类型 i 是否可用
        bool typeSupported = (typeFilter & (1 << i)) != 0;
        bool hasProperties =
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties;

        if (typeSupported && hasProperties) {
            return i;
        }
    }

    qWarning() << QStringLiteral("[Nv12Render_Vulkan] Failed to find suitable memory type.");
    return 0;
}

void Nv12Render_Vulkan::cleanupVulkanResources()
{
    // 渲染管线
    if (graphicsPipeline_ != VK_NULL_HANDLE) {
        vkDispatchTable_.fp_vkDestroyPipeline(vkDevice_.device, graphicsPipeline_, nullptr);
        graphicsPipeline_ = VK_NULL_HANDLE;
    }
    if (graphicsPipelineLayout_ != VK_NULL_HANDLE) {
        vkDispatchTable_.fp_vkDestroyPipelineLayout(vkDevice_.device, graphicsPipelineLayout_,
                                                    nullptr);
        graphicsPipelineLayout_ = VK_NULL_HANDLE;
    }
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDispatchTable_.fp_vkDestroyRenderPass(vkDevice_.device, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
    if (framebuffer_ != VK_NULL_HANDLE) {
        vkDispatchTable_.fp_vkDestroyFramebuffer(vkDevice_.device, framebuffer_, nullptr);
        framebuffer_ = VK_NULL_HANDLE;
    }

    // 采样器
    if (ycbcrSampler_ != VK_NULL_HANDLE) {
        vkDispatchTable_.fp_vkDestroySampler(vkDevice_.device, ycbcrSampler_, nullptr);
        ycbcrSampler_ = VK_NULL_HANDLE;
    }
    if (ycbcrConversion_ != VK_NULL_HANDLE) {
        vkDispatchTable_.fp_vkDestroySamplerYcbcrConversion(vkDevice_.device, ycbcrConversion_,
                                                            nullptr);
        ycbcrConversion_ = VK_NULL_HANDLE;
    }

    // RGBA输出纹理
    if (rgbaImageView_ != VK_NULL_HANDLE) {
        vkDispatchTable_.fp_vkDestroyImageView(vkDevice_.device, rgbaImageView_, nullptr);
        rgbaImageView_ = VK_NULL_HANDLE;
    }
    if (rgbaImage_ != VK_NULL_HANDLE) {
        vkDispatchTable_.fp_vkDestroyImage(vkDevice_.device, rgbaImage_, nullptr);
        rgbaImage_ = VK_NULL_HANDLE;
    }
    if (rgbaMemory_ != VK_NULL_HANDLE) {
        vkDispatchTable_.fp_vkFreeMemory(vkDevice_.device, rgbaMemory_, nullptr);
        rgbaMemory_ = VK_NULL_HANDLE;
    }

    // 描述符
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDispatchTable_.fp_vkDestroyDescriptorPool(vkDevice_.device, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDispatchTable_.fp_vkDestroyDescriptorSetLayout(vkDevice_.device, descriptorSetLayout_,
                                                         nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
    if (commandBuffer_ != VK_NULL_HANDLE && commandPool_ != VK_NULL_HANDLE) {
        vkDispatchTable_.fp_vkFreeCommandBuffers(vkDevice_.device, commandPool_, 1,
                                                 &commandBuffer_);
        commandBuffer_ = VK_NULL_HANDLE;
    }
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDispatchTable_.fp_vkDestroyCommandPool(vkDevice_.device, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }

    // 图形队列
    if (fence_ != VK_NULL_HANDLE) {
        vkDispatchTable_.fp_vkDestroyFence(vkDevice_.device, fence_, nullptr);
        fence_ = VK_NULL_HANDLE;
    }
}

void Nv12Render_Vulkan::cleanupOpenGLResources()
{
    if (glRGBATexture_) {
        glDeleteTextures(1, &glRGBATexture_);
        glRGBATexture_ = 0;
    }
    if (glMemoryObject_) {
        if (glDeleteMemoryObjectsEXT)
            glDeleteMemoryObjectsEXT(1, &glMemoryObject_);
        glMemoryObject_ = 0;
    }
}

#endif