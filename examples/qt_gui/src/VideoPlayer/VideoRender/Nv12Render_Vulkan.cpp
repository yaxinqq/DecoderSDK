#include "Nv12Render_Vulkan.h"

#ifdef VULKAN_AVAILABLE
#include <vulkan/vulkan.h>

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

    uniform sampler2D textureY;
    uniform sampler2D textureUV;

    varying vec2 textureOut;

    void main(void)
    {
        // 采样Y和UV纹理
        float y = texture2D(textureY, textureOut).r;
        vec2 uv = texture2D(textureUV, textureOut).rg;

        // 常量偏移和转换矩阵
        const vec3 yuv2rgb_ofs = vec3(0.0625, 0.5, 0.5);
        const mat3 yuv2rgb_mat = mat3(
            1.16438356,  0.0,           1.79274107,
            1.16438356, -0.21324861, -0.53290932,
            1.16438356,  2.11240178,  0.0
        );

        // YUV到RGB的转换
        vec3 rgb = (vec3(y, uv.r, uv.g) - yuv2rgb_ofs) * yuv2rgb_mat;
        gl_FragColor = vec4(rgb, 1.0);
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

// 全局或类内静态存储
static bool g_extLoaded = false;
static bool g_extLoadTried = false;

// 扩展函数指针
static PFNGLGENSEMAPHORESEXTPROC glGenSemaphoresEXT = nullptr;
static PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC glImportSemaphoreWin32HandleEXT = nullptr;
static PFNGLWAITSEMAPHOREEXTPROC glWaitSemaphoreEXT = nullptr;
static PFNGLCREATEMEMORYOBJECTSEXTPROC glCreateMemoryObjectsEXT = nullptr;
static PFNGLDELETEMEMORYOBJECTSEXTPROC glDeleteMemoryObjectsEXT = nullptr;
static PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC glImportMemoryWin32HandleEXT = nullptr;
static PFNGLNAMEDBUFFERSTORAGEMEMEXTPROC glNamedBufferStorageMemEXT = nullptr;
static PFNGLTEXTURESTORAGE2DPROC glTextureStorage2D = nullptr;
static PFNGLTEXTURESUBIMAGE2DPROC glTextureSubImage2D = nullptr;
static PFNGLTEXTUREPARAMETERIPROC glTextureParameteri = nullptr;
static PFNGLSIGNALSEMAPHOREEXTPROC glSignalSemaphoreEXT = nullptr;
static PFNGLCREATETEXTURESPROC glCreateTextures = nullptr;
static PFNGLDELETESEMAPHORESEXTPROC glDeleteSemaphoresEXT = nullptr;

bool loadExtFunctions(QOpenGLContext *ctx)
{
    // 已经加载过就返回结果
    if (g_extLoadTried)
        return g_extLoaded;

    g_extLoadTried = true;

    if (!ctx) {
        qWarning() << "loadExtFunctions: no context!";
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
                qWarning() << "Missing extension func:" << name;
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
    ok &= loadFunc(glNamedBufferStorageMemEXT, "glNamedBufferStorageMemEXT");
    ok &= loadFunc(glTextureStorage2D, "glTextureStorage2D");
    ok &= loadFunc(glTextureSubImage2D, "glTextureSubImage2D");
    ok &= loadFunc(glTextureParameteri, "glTextureParameteri");
    ok &= loadFunc(glSignalSemaphoreEXT, "glSignalSemaphoreEXT");
    ok &= loadFunc(glCreateTextures, "glCreateTextures");
    ok &= loadFunc(glDeleteSemaphoresEXT, "glDeleteSemaphoresEXT");

    g_extLoaded = ok;
    return ok;
}

struct QueueGuard {
    const decoder_sdk::Frame &f;
    uint32_t queueFamily;
    uint32_t queueIndex;
    QueueGuard(const decoder_sdk::Frame &f, uint32_t queueFamily, uint32_t queueIndex)
        : f(f)
        , queueFamily(queueFamily)
        , queueIndex(queueIndex)
    {
        f.lockVulkanQueue(queueFamily, queueIndex);
    }
    ~QueueGuard()
    {
        f.unlockVulkanQueue(queueFamily, queueIndex);
    }
};
} // namespace

Nv12Render_Vulkan::Nv12Render_Vulkan()
    : VideoRender(),
      vkInstance_{vulkan::getVkInstance()},
      vkPhysicalDevice_{vulkan::getVkPhysicalDevice()},
      vkDevice_{vulkan::getVkDevice()},
      vkInstanceDispatchTable_{vulkan::getInstanceDispatchTable()},
      vkDispatchTable_{vulkan::getDispatchTable()}
{
}

Nv12Render_Vulkan::~Nv12Render_Vulkan()
{
    vbo_.destroy();
    if (pbo_ > 0) {
        glDeleteBuffers(1, &pbo_);
        pbo_ = 0;
    }
    if (memObj_ > 0) {
        glDeleteMemoryObjectsEXT(1, &memObj_);
        memObj_ = 0;
    }

    if (commandPool_ != VK_NULL_HANDLE) {
        vkDispatchTable_.fp_vkDestroyCommandPool(vkDevice_.device, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }

    if (extBuffer_) {
        vkDispatchTable_.fp_vkDestroyBuffer(vkDevice_.device, extBuffer_, nullptr);
        extBuffer_ = VK_NULL_HANDLE;
    }
    if (extMemory_) {
        vkDispatchTable_.fp_vkFreeMemory(vkDevice_.device, extMemory_, nullptr);
        extMemory_ = VK_NULL_HANDLE;
    }
    shutdownExternalSemaphores();
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
    if (!loadExtFunctions(QOpenGLContext::currentContext()) || !vulkan::isVulkanAvaliable()) {
        return false;
    }

    if (!createCommandPool()) {
        return false;
    }

    uint64_t sizeY = 0, sizeUV = 0;
    if (!prepareExternalBuffer(frame.width(), frame.height(), sizeY, sizeUV)) {
        qWarning() << QStringLiteral("prepareExternalBuffer failed");
        return false;
    }

    void *hmem = externalBufferHandle();
    size_t total = (size_t)extBufferSize_;
    if (!memObj_ || memSize_ != total) {
        if (pbo_) {
            glDeleteBuffers(1, &pbo_);
            pbo_ = 0;
        }
        if (memObj_) {
            glDeleteMemoryObjectsEXT(1, &memObj_);
            memObj_ = 0;
        }
        glCreateMemoryObjectsEXT(1, &memObj_);
        glImportMemoryWin32HandleEXT(memObj_, (GLuint64)total, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                     hmem);
        glGenBuffers(1, &pbo_);
        glNamedBufferStorageMemEXT(pbo_, (GLsizeiptr)total, memObj_, 0);
        memSize_ = total;
    }

    return true;
}

bool Nv12Render_Vulkan::renderFrame(const decoder_sdk::Frame &frame)
{
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

    if (!glReadySem_ || !glCompleteSem_) {
        if (!initExternalSemaphores(frame)) {
            qWarning() << QStringLiteral("initExternalSemaphores failed");
            return false;
        }
        glGenSemaphoresEXT(1, &glReadySem_);
        glGenSemaphoresEXT(1, &glCompleteSem_);

        void *hReady = readySemaphoreHandle();
        void *hComplete = completeSemaphoreHandle();
        glImportSemaphoreWin32HandleEXT(glReadySem_, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, hReady);
        glImportSemaphoreWin32HandleEXT(glCompleteSem_, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, hComplete);
    }

    const auto &vkFrmae = guard.vkFrame;
    if (!vkFrmae || !semInitialized_) {
        return false;
    }

    for (int i = 0; i < 8 && vkFrmae->sem[i] != VK_NULL_HANDLE; ++i) {
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &vkFrmae->sem[i];
        waitInfo.pValues = &vkFrmae->sem_value[i];
        vkDispatchTable_.fp_vkWaitSemaphores(vkDevice_.device, &waitInfo, UINT64_MAX);
    }

    const int w = frame.width();
    const int h = frame.height();
    if (!copyImageToExternalBuffer(frame, vkFrmae, w, h)) {
        qWarning() << QStringLiteral("copyImageToExternalBuffer failed");
    }

    // GPU-side wait on ready semaphore
    const GLuint waitBuffers[1] = {pbo_};
    glWaitSemaphoreEXT(glReadySem_, 1, waitBuffers, 0, nullptr, nullptr);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_);

    GLuint texY = 0, texUV = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &texY);
    glCreateTextures(GL_TEXTURE_2D, 1, &texUV);
    glTextureStorage2D(texY, 1, GL_R8, w, h);
    glTextureStorage2D(texUV, 1, GL_RG8, w / 2, h / 2);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(texY, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, (const void *)(uintptr_t)0);
    glTextureSubImage2D(texUV, 0, 0, 0, w / 2, h / 2, GL_RG, GL_UNSIGNED_BYTE,
                        (const void *)(uintptr_t)extOffsetUV_);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glTextureParameteri(texY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(texY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(texY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(texY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(texUV, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(texUV, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(texUV, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(texUV, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 绘制
    drawFrame(texY, texUV);

    // GPU-side signal complete semaphore
    const GLuint signalBuffers[1] = {pbo_};
    glSignalSemaphoreEXT(glCompleteSem_, 1, signalBuffers, 0, nullptr, nullptr);
    for (int i = 0; i < 8 && vkFrmae->sem[i] != VK_NULL_HANDLE; ++i) {
        vkFrmae->sem_value[i]++;
        VkSemaphoreSignalInfo sig{};
        sig.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
        sig.semaphore = vkFrmae->sem[i];
        sig.value = vkFrmae->sem_value[i];
        vkDispatchTable_.fp_vkSignalSemaphore(vkDevice_.device, &sig);
    }

    glDeleteTextures(1, &texY);
    glDeleteTextures(1, &texUV);

    return true;
}

void Nv12Render_Vulkan::drawFrame(GLuint idY, GLuint idUV)
{
    program_.bind();
    vbo_.bind();
    program_.enableAttributeArray("vertexIn");
    program_.enableAttributeArray("textureIn");
    program_.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 2, 0);
    program_.setAttributeBuffer("textureIn", GL_FLOAT, 2 * 4 * sizeof(GLfloat), 2, 0);

    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, idY);

    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, idUV);

    program_.setUniformValue("textureY", 1);
    program_.setUniformValue("textureUV", 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    program_.disableAttributeArray("vertexIn");
    program_.disableAttributeArray("textureIn");
    vbo_.release();
    program_.release();
}

bool Nv12Render_Vulkan::prepareExternalBuffer(int w, int h, uint64_t &sizeY, uint64_t &sizeUV)
{
    sizeY = (uint64_t)w * (uint64_t)h;
    sizeUV = (uint64_t)(w / 2) * (uint64_t)(h / 2) * 2ull;
    uint64_t total = sizeY + sizeUV;
    if (extBuffer_ && extBufferSize_ >= total) {
        extBufferSize_ = total;
        extOffsetY_ = 0;
        extOffsetUV_ = sizeY;
        return true;
    }
    if (extBuffer_) {
        vkDispatchTable_.fp_vkDestroyBuffer(vkDevice_.device, extBuffer_, nullptr);
        extBuffer_ = VK_NULL_HANDLE;
    }
    if (extMemory_) {
        vkDispatchTable_.fp_vkFreeMemory(vkDevice_.device, extMemory_, nullptr);
        extMemory_ = VK_NULL_HANDLE;
    }

    VkExternalMemoryBufferCreateInfo extBuf{};
    extBuf.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    extBuf.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.pNext = &extBuf;
    bci.size = total;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkDispatchTable_.fp_vkCreateBuffer(vkDevice_.device, &bci, nullptr, &extBuffer_) !=
        VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    vkDispatchTable_.fp_vkGetBufferMemoryRequirements(vkDevice_.device, extBuffer_, &req);
    uint32_t memType = findMemoryTypeLocal(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memType == UINT32_MAX)
        return false;

    VkExportMemoryAllocateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &exportInfo;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memType;
    if (vkDispatchTable_.fp_vkAllocateMemory(vkDevice_.device, &mai, nullptr, &extMemory_) !=
        VK_SUCCESS)
        return false;
    if (vkDispatchTable_.fp_vkBindBufferMemory(vkDevice_.device, extBuffer_, extMemory_, 0) !=
        VK_SUCCESS)
        return false;

    extBufferSize_ = total;
    extOffsetY_ = 0;
    extOffsetUV_ = sizeY;
    return true;
}

uint32_t Nv12Render_Vulkan::findMemoryTypeLocal(uint32_t typeBits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties memProps{};
    vkInstanceDispatchTable_.fp_vkGetPhysicalDeviceMemoryProperties(
        vkPhysicalDevice_.physical_device, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

bool Nv12Render_Vulkan::initExternalSemaphores(const decoder_sdk::Frame &frame)
{
    if (semInitialized_) {
        return true;
    }

    VkExportSemaphoreCreateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &exportInfo;
    if (vkDispatchTable_.fp_vkCreateSemaphore(vkDevice_.device, &sci, nullptr, &semReady_) !=
        VK_SUCCESS) {
        return false;
    }

    if (vkDispatchTable_.fp_vkCreateSemaphore(vkDevice_.device, &sci, nullptr, &semComplete_) !=
        VK_SUCCESS) {
        return false;
    }

    // Initialize complete semaphore to signaled so first copy doesn't wait
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    VkSemaphoreSubmitInfo sig{};
    sig.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    sig.semaphore = semComplete_;
    sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    sig.deviceIndex = 0;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &sig;

    // 通过空提交信号量，让信号量进入“已信号”状态, 等待信号量的操作能立即通过，不阻塞
    {
        QueueGuard queueGuard(frame, graphicsQueueIndex_, 0);
        if (vkDispatchTable_.fp_vkQueueSubmit2(graphicsQueue_, 1, &submit, VK_NULL_HANDLE) !=
            VK_SUCCESS) {
            return false;
        }
    }

    semInitialized_ = true;
    return true;
}

void *Nv12Render_Vulkan::readySemaphoreHandle() const
{
    HANDLE h = nullptr;

    PFN_vkGetSemaphoreWin32HandleKHR pfn =
        (PFN_vkGetSemaphoreWin32HandleKHR)vkInstance_.fp_vkGetDeviceProcAddr(
            vkDevice_.device, "vkGetSemaphoreWin32HandleKHR");
    if (pfn) {
        VkSemaphoreGetWin32HandleInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
        info.semaphore = semReady_;
        info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

        pfn(vkDevice_.device, &info, &h);
    }

    return h;
}

void *Nv12Render_Vulkan::completeSemaphoreHandle() const
{
    HANDLE h = nullptr;

    PFN_vkGetSemaphoreWin32HandleKHR pfn =
        (PFN_vkGetSemaphoreWin32HandleKHR)vkInstance_.fp_vkGetDeviceProcAddr(
            vkDevice_.device, "vkGetSemaphoreWin32HandleKHR");
    if (pfn) {
        VkSemaphoreGetWin32HandleInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
        info.semaphore = semComplete_;
        info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

        pfn(vkDevice_.device, &info, &h);
    }

    return h;
}

void *Nv12Render_Vulkan::externalBufferHandle() const
{
    HANDLE h = nullptr;

    PFN_vkGetMemoryWin32HandleKHR pfn =
        (PFN_vkGetMemoryWin32HandleKHR)vkInstance_.fp_vkGetDeviceProcAddr(
            vkDevice_.device, "vkGetMemoryWin32HandleKHR");
    if (pfn) {
        VkMemoryGetWin32HandleInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
        info.memory = extMemory_;
        info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        pfn(vkDevice_.device, &info, &h);
    }

    return h;
}

void Nv12Render_Vulkan::shutdownExternalSemaphores()
{
    if (glReadySem_ > 0) {
        glDeleteSemaphoresEXT(1, &glReadySem_);
        glReadySem_ = 0;
    }

    if (glCompleteSem_ > 0) {
        glDeleteSemaphoresEXT(1, &glCompleteSem_);
        glCompleteSem_ = 0;
    }

    if (semReady_) {
        vkDispatchTable_.fp_vkDestroySemaphore(vkDevice_.device, semReady_, nullptr);
        semReady_ = VK_NULL_HANDLE;
    }

    if (semComplete_) {
        vkDispatchTable_.fp_vkDestroySemaphore(vkDevice_.device, semComplete_, nullptr);
        semComplete_ = VK_NULL_HANDLE;
    }

    semInitialized_ = false;
}

bool Nv12Render_Vulkan::copyImageToExternalBuffer(
    const decoder_sdk::Frame &frame, const std::shared_ptr<decoder_sdk::VulkanFrame> &vulkanFrame, int w, int h)
{
    if (!vulkanFrame || !extBuffer_)
        return false;

    VkCommandBufferAllocateInfo cbAlloc{};
    cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool = commandPool_;
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkDispatchTable_.fp_vkAllocateCommandBuffers(vkDevice_.device, &cbAlloc, &cmd) !=
        VK_SUCCESS)
        return false;

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkDispatchTable_.fp_vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
        return false;

    VkImageMemoryBarrier2 barriers[2]{};
    for (int i = 0; i < 2; i++) {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barriers[i].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barriers[i].srcAccessMask = 0;
        barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barriers[i].oldLayout = vulkanFrame->layout[0];
        barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[i].image = vulkanFrame->img[0];
        barriers[i].subresourceRange.baseMipLevel = 0;
        barriers[i].subresourceRange.levelCount = 1;
        barriers[i].subresourceRange.baseArrayLayer = 0;
        barriers[i].subresourceRange.layerCount = 1;
    }
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    uint32_t srcQ = vulkanFrame->queue_family[0];
    if (srcQ != VK_QUEUE_FAMILY_IGNORED) {
        barriers[0].srcQueueFamilyIndex = srcQ;
        barriers[0].dstQueueFamilyIndex = graphicsQueueIndex_;
        barriers[1].srcQueueFamilyIndex = srcQ;
        barriers[1].dstQueueFamilyIndex = graphicsQueueIndex_;
    } else {
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers = barriers;
    vkDispatchTable_.fp_vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy copyY{};
    copyY.bufferOffset = extOffsetY_;
    copyY.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    copyY.imageSubresource.mipLevel = 0;
    copyY.imageSubresource.baseArrayLayer = 0;
    copyY.imageSubresource.layerCount = 1;
    copyY.imageOffset = {0, 0, 0};
    copyY.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    VkBufferImageCopy copyUV{};
    copyUV.bufferOffset = extOffsetUV_;
    copyUV.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    copyUV.imageSubresource.mipLevel = 0;
    copyUV.imageSubresource.baseArrayLayer = 0;
    copyUV.imageSubresource.layerCount = 1;
    copyUV.imageOffset = {0, 0, 0};
    copyUV.imageExtent = {(uint32_t)(w / 2), (uint32_t)(h / 2), 1};

    vkDispatchTable_.fp_vkCmdCopyImageToBuffer(
        cmd, vulkanFrame->img[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, extBuffer_, 1, &copyY);
    vkDispatchTable_.fp_vkCmdCopyImageToBuffer(
        cmd, vulkanFrame->img[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, extBuffer_, 1, &copyUV);

    if (vkDispatchTable_.fp_vkEndCommandBuffer(cmd) != VK_SUCCESS)
        return false;

    if (!semInitialized_)
        initExternalSemaphores(frame);

    VkSemaphoreSubmitInfo waits{};
    waits.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waits.semaphore = semComplete_;
    waits.value = 0;
    waits.stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    waits.deviceIndex = 0;
    VkSemaphoreSubmitInfo sigs{};
    sigs.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    sigs.semaphore = semReady_;
    sigs.value = 0;
    sigs.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    sigs.deviceIndex = 0;
    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = cmd;
    VkSubmitInfo2 submit2{};
    submit2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit2.waitSemaphoreInfoCount = 1;
    submit2.pWaitSemaphoreInfos = &waits;
    submit2.commandBufferInfoCount = 1;
    submit2.pCommandBufferInfos = &cmdInfo;
    submit2.signalSemaphoreInfoCount = 1;
    submit2.pSignalSemaphoreInfos = &sigs;

    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence f{};
    vkDispatchTable_.fp_vkCreateFence(vkDevice_.device, &fi, nullptr, &f);
    {
        QueueGuard queueGuard(frame, graphicsQueueIndex_, 0);
        vkDispatchTable_.fp_vkQueueSubmit2(graphicsQueue_, 1, &submit2, f);
    }
    vkDispatchTable_.fp_vkWaitForFences(vkDevice_.device, 1, &f, VK_TRUE, UINT64_MAX);
    vkDispatchTable_.fp_vkDestroyFence(vkDevice_.device, f, nullptr);
    vkDispatchTable_.fp_vkFreeCommandBuffers(vkDevice_.device, commandPool_, 1, &cmd);
    return true;
}

bool Nv12Render_Vulkan::createCommandPool()
{
    auto graphicsQueueIndex = vkDevice_.get_queue_index(vkb::QueueType::graphics);
    if (!graphicsQueueIndex.has_value()) {
        qWarning() << QStringLiteral("Failed to get graphics queue index");
        return false;
    }
    graphicsQueueIndex_ = graphicsQueueIndex.value();
    graphicsQueue_ = vkDevice_.get_queue(vkb::QueueType::graphics).value();

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueIndex_;

    if (vkDispatchTable_.fp_vkCreateCommandPool(vkDevice_.device, &poolInfo, nullptr,
                                                &commandPool_) != VK_SUCCESS) {
        qWarning() << QStringLiteral("Failed to create command pool!");
        return false;
    }

    return true;
}

#endif