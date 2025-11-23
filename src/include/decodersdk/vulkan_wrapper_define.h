#ifndef DECODER_SDK_VULKAN_WRAPPER_DEFINE_H
#define DECODER_SDK_VULKAN_WRAPPER_DEFINE_H

#include <vulkan/vulkan_core.h>

// 前向声明，避免直接依赖FFmpeg头文件
struct AVHWDeviceContext;

namespace decoder_sdk {

// FFMpeg AVVulkanDeviceQueueFamily
typedef struct VulkanDeviceQueueFamily {
    /* Queue family index */
    int idx = -1;
    /* Number of queues in the queue family in use */
    int num = 0;
    /* Queue family capabilities. Must be non-zero.
     * Flags may be removed to indicate the queue family may not be used
     * for a given purpose. */
    VkQueueFlagBits flags;
    /* Vulkan implementations are allowed to list multiple video queues
     * which differ in what they can encode or decode. */
    VkVideoCodecOperationFlagBitsKHR video_caps;
} VulkanDeviceQueueFamily;

// FFMpeg AVVulkanDeviceContext的定义
/**
 * Main Vulkan context, allocated as AVHWDeviceContext.hwctx.
 * All of these can be set before init to change what the context uses
 */
typedef struct VulkanDeviceContext {
    /**
     * Custom memory allocator, else NULL
     */
    const VkAllocationCallbacks *alloc = nullptr;
    /**
     * Pointer to a vkGetInstanceProcAddr loading function.
     * If unset, will dynamically load and use libvulkan.
     */
    PFN_vkGetInstanceProcAddr get_proc_addr;
    /**
     * Vulkan instance. Must be at least version 1.1.
     */
    VkInstance inst;
    /**
     * Physical device
     */
    VkPhysicalDevice phys_dev;
    /**
     * Active device
     */
    VkDevice act_dev;

    /**
     * This structure should be set to the set of features that present and enabled
     * during device creation. When a device is created by FFmpeg, it will default to
     * enabling all that are present of the shaderImageGatherExtended,
     * fragmentStoresAndAtomics, shaderInt64 and vertexPipelineStoresAndAtomics features.
     */
    VkPhysicalDeviceFeatures2 device_features;

    /**
     * Enabled instance extensions.
     * If supplying your own device context, set this to an array of strings, with
     * each entry containing the specified Vulkan extension string to enable.
     * Duplicates are possible and accepted.
     * If no extensions are enabled, set these fields to NULL, and 0 respectively.
     */
    const char *const *enabled_inst_extensions;
    int nb_enabled_inst_extensions;

    /**
     * Enabled device extensions. By default, VK_KHR_external_memory_fd,
     * VK_EXT_external_memory_dma_buf, VK_EXT_image_drm_format_modifier,
     * VK_KHR_external_semaphore_fd and VK_EXT_external_memory_host are enabled if found.
     * If supplying your own device context, these fields takes the same format as
     * the above fields, with the same conditions that duplicates are possible
     * and accepted, and that NULL and 0 respectively means no extensions are enabled.
     */
    const char *const *enabled_dev_extensions;
    int nb_enabled_dev_extensions;

    /**
     * Queue families used. Must be preferentially ordered. List may contain
     * duplicates.
     *
     * For compatibility reasons, all the enabled queue families listed above
     * (queue_family_(tx/comp/encode/decode)_index) must also be included in
     * this list until they're removed after deprecation.
     */
    VulkanDeviceQueueFamily qf[64];
    int nb_qf;

    /**
     * Locks a queue, preventing other threads from submitting any command
     * buffers to this queue.
     * If set to NULL, will be set to lavu-internal functions that utilize a
     * mutex.
     */
    void (*lock_queue)(struct AVHWDeviceContext *ctx, uint32_t queue_family, uint32_t index);

    /**
     * Similar to lock_queue(), unlocks a queue. Must only be called after locking.
     */
    void (*unlock_queue)(struct AVHWDeviceContext *ctx, uint32_t queue_family, uint32_t index);
} VulkanDeviceContext;

// FFMpeg AVVkFrame的定义
/*
 * Frame structure, the VkFormat of the image will always match
 * the pool's sw_format.
 * All frames, imported or allocated, will be created with the
 * VK_IMAGE_CREATE_ALIAS_BIT flag set, so the memory may be aliased if needed.
 *
 * If all three queue family indices in the device context are the same,
 * images will be created with the EXCLUSIVE sharing mode. Otherwise, all images
 * will be created using the CONCURRENT sharing mode.
 *
 * @note the size of this structure is not part of the ABI, to allocate
 * you must use @av_vk_frame_alloc().
 */
typedef struct VulkanFrame {
    /**
     * Vulkan images to which the memory is bound to.
     * May be one for multiplane formats, or multiple.
     */
    VkImage img[8];

    /**
     * Tiling for the frame.
     */
    VkImageTiling tiling;

    /**
     * Memory backing the images. Either one, or as many as there are planes
     * in the sw_format.
     * In case of having multiple VkImages, but one memory, the offset field
     * will indicate the bound offset for each image.
     */
    VkDeviceMemory mem[8];
    size_t size[8];

    /**
     * OR'd flags for all memory allocated
     */
    VkMemoryPropertyFlagBits flags;

    /**
     * Updated after every barrier. One per VkImage.
     */
    VkAccessFlagBits access[8];
    VkImageLayout layout[8];

    /**
     * Synchronization timeline semaphores, one for each VkImage.
     * Must not be freed manually. Must be waited on at every submission using
     * the value in sem_value, and must be signalled at every submission,
     * using an incremented value.
     */
    VkSemaphore sem[8];

    /**
     * Up to date semaphore value at which each image becomes accessible.
     * One per VkImage.
     * Clients must wait on this value when submitting a command queue,
     * and increment it when signalling.
     */
    uint64_t sem_value[8];

    /**
     * Describes the binding offset of each image to the VkDeviceMemory.
     * One per VkImage.
     */
    ptrdiff_t offset[8];

    /**
     * Queue family of the images. Must be VK_QUEUE_FAMILY_IGNORED if
     * the image was allocated with the CONCURRENT concurrency option.
     * One per VkImage.
     */
    uint32_t queue_family[8];

    /**
     * AVVKFrame pointer
     */
    uint8_t *avvkframePtr = nullptr;
} VulkanFrame;

} // namespace decoder_sdk

#endif // DECODER_SDK_VULKAN_WRAPPER_DEFINE_H